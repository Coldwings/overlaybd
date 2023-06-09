/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "registryfs.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <photon/common/utility.h>
#include <photon/common/timeout.h>
#include <photon/common/iovector.h>
#include <photon/common/estring.h>
#include <photon/common/expirecontainer.h>
#include <photon/common/callback.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/virtual-file.h>
#include <photon/net/http/client.h>
#include <photon/net/utils.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace photon::fs;
using namespace photon::net::http;

static const estring kDockerRegistryAuthChallengeKeyValuePrefix = "www-authenticate";
static const estring kAuthHeaderKey = "Authorization";
static const estring kBearerAuthPrefix = "Bearer ";
static const uint64_t kMinimalTokenLife = 30L * 1000 * 1000; // token lives atleast 30s
static const uint64_t kMinimalAUrlLife = 300L * 1000 * 1000; // actual_url lives atleast 300s
static const uint64_t kMinimalMetaLife = 300L * 1000 * 1000; // actual_url lives atleast 300s

using HTTP_OP = photon::net::http::Client::OperationOnStack<64 * 1024 - 1>;

static std::unordered_map<estring_view, estring_view> str_to_kvmap(const estring &src) {
    std::unordered_map<estring_view, estring_view> ret;
    for (const auto &token : src.split(',')) {
        auto pos = token.find_first_of('=');
        auto key = token.substr(0, pos);
        auto val = token.substr(pos + 1).trim('\"');
        ret.emplace(key, val);
    }
    return ret;
}

enum class UrlMode {
    Redirect,
    Self
};
struct UrlInfo {
    UrlMode mode;
    estring info;
};

class RegistryFSImpl_v2 : public RegistryFS {
public:
    UNIMPLEMENTED_POINTER(IFile *creat(const char *, mode_t) override);
    UNIMPLEMENTED(int mkdir(const char *, mode_t) override);
    UNIMPLEMENTED(int rmdir(const char *) override);
    UNIMPLEMENTED(int link(const char *, const char *) override);
    UNIMPLEMENTED(int rename(const char *, const char *) override);
    UNIMPLEMENTED(int chmod(const char *, mode_t) override);
    UNIMPLEMENTED(int chown(const char *, uid_t, gid_t) override);
    UNIMPLEMENTED(int statfs(const char *path, struct statfs *buf) override);
    UNIMPLEMENTED(int statvfs(const char *path, struct statvfs *buf) override);
    UNIMPLEMENTED(int lstat(const char *path, struct stat *buf) override);
    UNIMPLEMENTED(int access(const char *pathname, int mode) override);
    UNIMPLEMENTED(int truncate(const char *path, off_t length) override);
    UNIMPLEMENTED(int syncfs() override);
    UNIMPLEMENTED(int unlink(const char *filename) override);
    UNIMPLEMENTED(int lchown(const char *pathname, uid_t owner, gid_t group) override);
    UNIMPLEMENTED_POINTER(DIR *opendir(const char *) override);
    UNIMPLEMENTED(int symlink(const char *meta_pathname, const char *pathname) override);
    UNIMPLEMENTED(ssize_t readlink(const char *filename, char *buf, size_t bufsize) override);
    UNIMPLEMENTED(int utime(const char *path, const struct utimbuf *file_times) override);
    UNIMPLEMENTED(int utimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int lutimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int mknod(const char *path, mode_t mode, dev_t dev) override);

    virtual IFile *open(const char *pathname, int flags) override;

    virtual IFile *open(const char *pathname, int flags, mode_t) override {
        return open(pathname, flags); // ignore mode
    }

    RegistryFSImpl_v2(PasswordCB callback, const char *caFile, uint64_t timeout)
        : m_callback(callback), m_caFile(caFile), m_timeout(timeout),
          m_meta_size(kMinimalMetaLife), m_scope_token(kMinimalTokenLife),
          m_url_info(kMinimalAUrlLife) {
        m_client = new_http_client();
    }

    ~RegistryFSImpl_v2() {
        delete m_client;
    }

    long get_data(const estring &url, off_t offset, size_t count, uint64_t timeout, HTTP_OP &op) {
        Timeout tmo(timeout);
        long ret = 0;
        UrlInfo *actual_info = m_url_info.acquire(url, [&]() -> UrlInfo * {
            return get_actual_url(url, tmo.timeout(), ret);
        });

        if (actual_info == nullptr)
            return ret;

        estring *actual_url = (estring*)&url;
        if (actual_info->mode == UrlMode::Redirect)
            actual_url = &actual_info->info;
        //use p2p proxy
        estring accelerate_url;
        if(m_accelerate.size() > 0) {
            accelerate_url = estring().appends(m_accelerate, "/", *actual_url);
            actual_url = &accelerate_url;
            LOG_DEBUG("p2p_url: `", *actual_url);
        }

        op.req.reset(Verb::GET, *actual_url);
        // set token if needed
        if (actual_info->mode == UrlMode::Self && !actual_info->info.empty()) {
            op.req.headers.insert(kAuthHeaderKey, "Bearer ");
            op.req.headers.value_append(actual_info->info);
        }
        op.req.headers.range(offset, offset + count - 1);
        op.set_enable_proxy(m_client->has_proxy());
        op.retry = 0;
        op.timeout = tmo.timeout();
        m_client->call(&op);

        if (op.status_code == 200 || op.status_code == 206) {
            m_url_info.release(url);
            return ret;
        }

        m_url_info.release(url, true);
        LOG_ERROR_RETURN(0, ret, "Failed to fetch data ", VALUE(url), VALUE(op.status_code), VALUE(ret));
    }

    int stat(const char *path, struct stat *buf) override {
        auto ctor = [&]() -> size_t * {
            auto file = open(path, 0);
            if (file == nullptr)
                return nullptr;
            DEFER(delete file);
            struct stat buf;
            if (file->fstat(&buf) < 0)
                return nullptr;
            return new size_t(buf.st_size);
        };
        auto meta = m_meta_size.acquire(path, ctor);
        if (meta == nullptr)
            return -1;
        DEFER(m_meta_size.release(path));
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | S_IREAD;
        buf->st_size = *meta;
        return 0;
    }

    UrlInfo* get_actual_url(const estring &url, uint64_t timeout, long &code) {

        Timeout tmo(timeout);
        estring authurl, scope;
        estring *token = nullptr;
        if (get_scope_auth(url, &authurl, &scope, tmo.timeout()) < 0)
            return nullptr;

        if (!scope.empty()) {
            token = m_scope_token.acquire(scope, [&]() -> estring * {
                estring *token = new estring();
                auto ret = m_callback(url.data());
                if (!authenticate(authurl, ret.first, ret.second, token, tmo.timeout())) {
                    code = 401;
                    delete token;
                    return nullptr;
                }
                return token;
            });
            if (token == nullptr)
                LOG_ERROR_RETURN(0, nullptr, "Failed to get token");
        }

        HTTP_OP op(m_client, Verb::GET, url);
        op.follow = 0;
        op.retry = 0;
        op.req.headers.insert(kAuthHeaderKey, "Bearer ");
        op.req.headers.value_append(*token);
        op.timeout = tmo.timeout();
        op.call();
        if (op.status_code == 401 || op.status_code == 403) {
            LOG_WARN("Token invalid, try refresh password next time");
        }
        if (300 <= op.status_code && op.status_code < 400) {
            // pass auth, redirect to source
            auto location = op.resp.headers["Location"];
            if (!scope.empty())
                m_scope_token.release(scope);
            return new UrlInfo{UrlMode::Redirect, location};
        }
        if (op.status_code == 200) {
            UrlInfo *info = new UrlInfo{UrlMode::Self, ""};
            if (token && !token->empty())
                info->info = kBearerAuthPrefix + *token;
            if (!scope.empty())
                m_scope_token.release(scope);
            return info;
        }

        // unexpected situation
        if (!scope.empty())
            m_scope_token.release(scope, true);
        LOG_ERROR_RETURN(0, nullptr, "Failed to get actual url, status_code=` ", op.status_code, VALUE(url));
    }

    virtual int setAccelerateAddress(const char* addr = "") override {
        m_accelerate = estring(addr);
        return 0;
    }

protected:
    PasswordCB m_callback;
    estring m_accelerate;
    estring m_caFile;
    uint64_t m_timeout;
    photon::net::http::Client* m_client;
    ObjectCache<estring, size_t *> m_meta_size;
    ObjectCache<estring, estring *> m_scope_token;
    ObjectCache<estring, UrlInfo *> m_url_info;

    int get_scope_auth(const estring &url, estring *authurl, estring *scope, uint64_t timeout) {
        Timeout tmo(timeout);

        HTTP_OP op(m_client, Verb::GET, url);
        op.follow = 0;
        op.retry = 0;
        op.req.headers.range(0, 0);
        op.timeout = tmo.timeout();
        op.call();
        if (op.status_code == -1)
            LOG_ERROR_RETURN(ENOENT, -1, "connection failed");

        // going to challenge
        if (op.status_code != 401 && op.status_code != 403) {
            // no token request accepted, seems token not need;
            return 0;
        }

        auto it = op.resp.headers.find(kDockerRegistryAuthChallengeKeyValuePrefix);
        if (it == op.resp.headers.end())
            LOG_ERROR_RETURN(EINVAL, -1, "no auth header in response");
        estring challengeLine = it.second();
        if (!challengeLine.starts_with(kBearerAuthPrefix))
            LOG_ERROR_RETURN(EINVAL, -1, "auth string shows not bearer auth, ",
                             VALUE(challengeLine));
        challengeLine = challengeLine.substr(kBearerAuthPrefix.length());
        auto kv = str_to_kvmap(challengeLine);
        if (kv.find("realm") == kv.end() || kv.find("service") == kv.end() ||
            kv.find("scope") == kv.end()) {
            LOG_ERROR_RETURN(EINVAL, -1, "authentication challenge failed with `",
                             challengeLine);
        }
        *scope = estring(kv["scope"]);
        *authurl = estring().appends(kv["realm"], "?service=", kv["service"],
                    "&scope=", kv["scope"]);
        return 0;
    }

    int parse_token(const estring &jsonStr, estring *token) {
        rapidjson::Document d;
        if (d.Parse(jsonStr.c_str()).HasParseError())
            LOG_ERROR_RETURN(0, -1, "JSON parse failed");
        if (d.HasMember("token"))
            *token = d["token"].GetString();
        else if (d.HasMember("access_token"))
            *token = d["access_token"].GetString();
        else
            LOG_ERROR_RETURN(0, -1, "JSON has no 'token' or 'access_token' member");
        LOG_DEBUG("get token", VALUE(*token));
        return 0;
    }

    bool authenticate(const estring &authurl, std::string &username, std::string &password,
                      estring *token, uint64_t timeout) {
        Timeout tmo(timeout);
        estring userpwd_b64;
        photon::net::Base64Encode(estring().appends(username, ":", password), userpwd_b64);
        HTTP_OP op(m_client, Verb::GET, authurl);
        op.follow = 0;
        op.retry = 0;
        if (!username.empty()) {
            op.req.headers.insert(kAuthHeaderKey, "Basic ");
            op.req.headers.value_append(userpwd_b64);
        }
        op.timeout = tmo.timeout();
        op.call();
        if (op.status_code != 200) {
            LOG_ERROR_RETURN(EPERM, false, "invalid key");
        }
        estring body;
        body.resize(16 *1024);
        auto len = op.resp.read((void*)body.data(), 16 *1024);
        body.resize(len);
        if (op.status_code == 200 && parse_token(body, token) == 0)
            return true;
        LOG_ERROR_RETURN(EPERM, false, "auth failed, response code=` ", op.status_code, VALUE(authurl));
    }

}; // namespace FileSystem

class RegistryFileImpl_v2 : public photon::fs::VirtualReadOnlyFile {
public:
    estring m_url;
    RegistryFSImpl_v2 *m_fs;
    uint64_t m_timeout = -1;
    size_t m_filesize = 0;

    RegistryFileImpl_v2(const char *url, RegistryFSImpl_v2 *fs, uint64_t timeout)
        : m_url(url), m_fs(fs), m_timeout(timeout) {}

    virtual IFileSystem *filesystem() override {
        return m_fs;
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) {
        if (m_filesize == 0) {
            struct stat stat;
            auto stret = fstat(&stat);
            if (stret < 0)
                return -1;
            m_filesize = stat.st_size;
        }
        auto filesize = m_filesize;
        int retry = 3;
        Timeout tmo(m_timeout);

    again:
        iovector_view view((struct iovec*)iov, iovcnt);
        auto count = view.sum();
        if (count + offset > filesize)
            count = filesize - offset;
        LOG_DEBUG("pulling blob from registry: ", VALUE(m_url), VALUE(offset), VALUE(count));

        HTTP_OP op;
        auto ret = m_fs->get_data(m_url, offset, count, tmo.timeout(), op);
        if (op.status_code != 200 && op.status_code != 206) {
            ERRNO eno;
            if (tmo.expire() < photon::now) {
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "timed out in preadv ", VALUE(m_url), VALUE(offset));
            }
            if (retry--) {
                LOG_WARN("failed to perform HTTP GET, going to retry ", VALUE(op.status_code), VALUE(offset),
                         VALUE(count), VALUE(ret), eno);
                photon::thread_usleep(1000);
                goto again;
            } else {
                LOG_ERROR_RETURN(ENOENT, -1, "failed to perform HTTP GET ", VALUE(m_url),
                                 VALUE(offset));
            }
        }
        return op.resp.readv(iov, iovcnt);
    }

    int64_t get_length(uint64_t timeout = -1) {
        Timeout tmo(timeout);
        int retry = 3;
    again:
        HTTP_OP op;
        auto ret = m_fs->get_data(m_url, 0, 1, tmo.timeout(), op);
        if (op.status_code != 200 && op.status_code != 206) {
            if (tmo.expire() < photon::now)
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "get meta timedout");

            if (op.status_code == 401 || op.status_code == 403) {
                if (retry--)
                    goto again;
                LOG_ERROR_RETURN(EPERM, -1, "Authorization failed");
            }
            if (retry--)
                goto again;
            LOG_ERROR_RETURN(ENOENT, -1, "failed to get meta from server");
        }
        return op.resp.resource_size();
    }

    virtual int fstat(struct stat *buf) override {
        if (m_filesize == 0) {
            auto ret = get_length(m_timeout);
            if (ret < 0)
                return -1;
            m_filesize = ret;
        }
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | S_IREAD;
        buf->st_size = m_filesize;
        return 0;
    }

};

inline IFile *RegistryFSImpl_v2::open(const char *pathname, int) {
    auto file = new RegistryFileImpl_v2(pathname, (RegistryFSImpl_v2 *)this, m_timeout);
    struct stat buf;
    int ret = file->fstat(&buf);
    if (ret < 0) {
        delete file;
        LOG_ERROR_RETURN(0, nullptr, "failed to open and stat registry file `, ret `", pathname, ret);
    }
    return file;
}

IFileSystem *new_registryfs_v2(PasswordCB callback, const char *caFile, uint64_t timeout) {
    if (!callback)
        LOG_ERROR_RETURN(EINVAL, nullptr, "password callback not set");
    return new RegistryFSImpl_v2(callback, caFile ? caFile : "", timeout);
}
