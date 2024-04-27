FROM mcr.microsoft.com/devcontainers/universal:2-linux

ARG ZIG_VERSION="0.12.0"
WORKDIR /home/codespace
ADD --chown=codespace:codespace install-zig.sh install-zig.sh
RUN chmod +x install-zig.sh && ./install-zig.sh ${ZIG_VERSION} ${ZIG_CHECKSUM}}
