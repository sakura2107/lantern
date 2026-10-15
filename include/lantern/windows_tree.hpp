#pragma once
#include "windows_client.hpp"
namespace lantern {
std::optional<uint64_t> recover_committed_tree(const TransferTask&,const fs::path&,Cancel&);
void finalize_tree_journal(const fs::path&);
void transfer_tree(Client&,const TransferTask&,const fs::path& manifest,Cancel&,std::function<void(uint64_t,uint64_t)>);
}
