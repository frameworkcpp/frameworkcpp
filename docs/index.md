---
layout: home

hero:
  name: "FrameworkCpp"
  text: "Build your own Real-Time systems in C++"
  actions:
    - theme: brand
      text: Getting Started
      link: /getting-started
    - theme: alt
      text: Source Code
      link: https://github.com/frameworkcpp/frameworkcpp

features:
  - title: Header-only C++20
    details: Everything lives in include/frameworkcpp. Add the include path to the compiler and you are done — nothing to link, nothing to build.
  - title: Real-time friendly
    details: No dynamic allocation at runtime, no locks on fast paths, and memory budgets you can reason about ahead of time.
  - title: Hardened by default
    details: The CI runs the whole suite under ASAN, TSAN and UBSAN on multiple architectures, so the behaviour you ship is the behaviour that was tested.
  - title: Standard tooling
    details: CMake presets, CPack packages, Docker images, and a semantic version header — no magic, plain build system integration.
---