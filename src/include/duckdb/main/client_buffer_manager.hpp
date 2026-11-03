//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class BufferManager;
class RuntimeServices;

//! Creates the buffer manager of a client: wraps the buffer manager of the database to forward the services of the
//! client and to track the memory it allocates
unique_ptr<BufferManager> CreateClientBufferManager(RuntimeServices &services, BufferManager &buffer_manager);

} // namespace duckdb
