FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    libjson-glib-dev \
    libsoup-3.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    gdb \
    vim \
    wget \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -ms /bin/bash vscode
USER vscode
WORKDIR /home/vscode

RUN wget -O /home/vscode/test.wav https://static.deepgram.com/examples/deep-learning-podcast-clip.wav