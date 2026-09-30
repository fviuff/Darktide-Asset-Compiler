#pragma once

#include <mutex>
#include <string>

namespace physx { class PxFoundation; }

namespace dtglb::stingray::physics {

// PhysX permits one foundation per process. Cooking and collection serialization
// share it, and serialize access to the SDK and its diagnostic callback.
std::recursive_mutex& physx_context_mutex();
// Borrowed until process shutdown; callers must not release it. Hold the mutex
// for the complete lifetime of any SDK objects created using this foundation.
physx::PxFoundation* physics_foundation(std::string& error);
// Call while holding the same mutex. Warnings do not count as SDK failures.
void physics_clear_errors();
std::string physics_last_error();

} // namespace dtglb::stingray::physics
