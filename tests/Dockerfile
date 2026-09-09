FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates \
    libssl-dev libplist-dev libavahi-compat-libdnssd-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
    python3 ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
