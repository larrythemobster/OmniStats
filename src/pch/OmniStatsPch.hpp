#pragma once

// Target Windows 10+ and lean headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Asio must be included before windows.h (or with WIN32_LEAN_AND_MEAN)
// so Winsock 2 is used and winsock.h (Winsock 1.1) is never pulled in.
#include <asio.hpp>
#include <windows.h>

// rpcndr.h defines 'small' as a macro which collides with identifiers.
#ifdef small
#undef small
#endif

#include <nlohmann/json.hpp>

// Common standard library headers
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
