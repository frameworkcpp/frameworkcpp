// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Umbrella header: pulls in every shipped module and their version entry
// points. Prefer the granular headers (e.g. <frameworkcpp/arena/arena.hpp>) for
// faster builds when you only need one module.
//
// The reactor module is NOT included here: it talks to io_uring through
// liburing, so it needs an extra link flag. Include it explicitly:
//
//     #include <frameworkcpp/reactor/reactor.hpp>   // link with -luring

#include "frameworkcpp/arena/arena.hpp"
#include "frameworkcpp/arena/module_memory.hpp"
#include "frameworkcpp/arena/version.hpp"
#include "frameworkcpp/environment/environment.hpp"
#include "frameworkcpp/environment/version.hpp"
