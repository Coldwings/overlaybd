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

#pragma once
#include <stdint.h>
#include <string>
#include <photon/common/callback.h>
#include <photon/fs/filesystem.h>

class RegistryFS : public photon::fs::IFileSystem {
public:
    virtual int setAccelerateAddress(const char* addr = "") = 0;
};

using PasswordCB = Delegate<std::pair<std::string, std::string>, const char *>;

extern "C" {
photon::fs::IFileSystem *new_registryfs_v1(PasswordCB callback,
                                           const char *caFile = nullptr,
                                           uint64_t timeout = -1);

photon::fs::IFileSystem *new_registryfs_v2(PasswordCB callback,
                                           const char *caFile = nullptr,
                                           uint64_t timeout = -1);

photon::fs::IFile* new_registry_uploader(photon::fs::IFile *lfile,
                                         std::string &upload_url,
                                         std::string &username, std::string &password,
                                         uint64_t timeout,
                                         ssize_t upload_bs = -1);
}
