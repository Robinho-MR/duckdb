#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {
namespace capiv2 {

//! Memory reserved in the buffer manager of a database, released when the reservation is destroyed.
class CV2MemoryReservation {
public:
	CV2MemoryReservation(BufferManager &buffer_manager, idx_t size) : buffer_manager(buffer_manager), size(size) {
	}
	CV2MemoryReservation(const CV2MemoryReservation &) = delete;
	CV2MemoryReservation &operator=(const CV2MemoryReservation &) = delete;
	~CV2MemoryReservation() {
		buffer_manager.FreeReservedMemory(size);
	}

	void Resize(idx_t new_size) {
		if (new_size > size) {
			buffer_manager.ReserveMemory(new_size - size);
		} else {
			buffer_manager.FreeReservedMemory(size - new_size);
		}
		size = new_size;
	}

	idx_t GetSize() const {
		return size;
	}

private:
	BufferManager &buffer_manager;
	idx_t size;
};

static auto Convert(duckdb_v2_memory_reservation_handle reservation) -> CV2MemoryReservation * {
	return reinterpret_cast<CV2MemoryReservation *>(reservation);
}
static auto Convert(CV2MemoryReservation *reservation) -> duckdb_v2_memory_reservation_handle {
	return reinterpret_cast<duckdb_v2_memory_reservation_handle>(reservation);
}

} // namespace capiv2
} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_context_reserve_memory(duckdb_v2_context_handle context, idx_t size,
                                                 duckdb_v2_memory_reservation_handle *reservation,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(reservation);
	*reservation = nullptr;
	DUCKDB_CHECK_ARG(context);
	return WithErrorHandler(err, [&]() {
		auto &buffer_manager = duckdb::BufferManager::GetBufferManager(Convert(context)->factory.GetDatabase());
		buffer_manager.ReserveMemory(size);
		*reservation = Convert(new CV2MemoryReservation(buffer_manager, size));
	});
}

DUCKDB_V2_ERROR duckdb_v2_memory_reservation_resize(duckdb_v2_memory_reservation_handle reservation, idx_t size,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(reservation);
	return WithErrorHandler(err, [&]() { Convert(reservation)->Resize(size); });
}

DUCKDB_V2_ERROR duckdb_v2_memory_reservation_get_size(duckdb_v2_memory_reservation_handle reservation, idx_t *size,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(size);
	*size = 0;
	DUCKDB_CHECK_ARG(reservation);
	return WithErrorHandler(err, [&]() { *size = Convert(reservation)->GetSize(); });
}

DUCKDB_V2_ERROR duckdb_v2_memory_reservation_destroy(duckdb_v2_memory_reservation_handle *reservation) {
	if (reservation && *reservation) {
		delete Convert(*reservation);
		*reservation = nullptr;
	}
	return DUCKDB_V2_ERROR_NONE;
}
