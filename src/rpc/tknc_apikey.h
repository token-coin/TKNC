#ifndef TKN_RPC_TKN_APIKEY_H
#define TKN_RPC_TKN_APIKEY_H

#include <rpc/server.h>
#include <util/fs.h>

class CAPIKeyDB;

void InitAPIKeyDB(const fs::path& data_dir);
void ShutdownAPIKeyDB();
CAPIKeyDB* GetAPIKeyDB();

#endif // TKN_RPC_TKN_APIKEY_H
