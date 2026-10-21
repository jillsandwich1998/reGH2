# Build environment for the distributable SDK / AppImage: Ubuntu 22.04 keeps the glibc floor at 2.35, so the result runs on
# every mainstream distro from 2022 on (and on SteamOS). Same recipe as the ReXGlue release build.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl wget gnupg git git-lfs software-properties-common lsb-release xz-utils zip unzip file \
    && git config --global --add safe.directory '*'
RUN wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 20 \
    && apt-get install -y clang-20 lld-20 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 200 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 200
# jammy's libstdc++ 11 has no <format> and its cmake is older than the SDK's minimum: use the toolchain PPA and Kitware.
RUN wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc > /etc/apt/trusted.gpg.d/lunarg.asc \
    && wget -qO /etc/apt/sources.list.d/lunarg-vulkan-jammy.list http://packages.lunarg.com/vulkan/lunarg-vulkan-jammy.list \
    && curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F" | gpg --dearmor -o /usr/share/keyrings/toolchain-r.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main" > /etc/apt/sources.list.d/toolchain-r.list \
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware.gpg] https://apt.kitware.com/ubuntu/ jammy main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y cmake ninja-build build-essential g++-13 autoconf python3 python3-venv \
      libgtk-3-dev libx11-xcb-dev libxss-dev vulkan-sdk libwayland-dev libwayland-bin wayland-protocols \
      libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
