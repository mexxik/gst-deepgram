# gst-deepgram

A GStreamer plugin that sends mono, 16kHz, S16LE raw PCM audio to [Deepgram](https://deepgram.com)'s real-time transcription API over WebSockets.

## Features

* GStreamer sink element: `deepgramsink`
* Real-time transcription using Deepgram API
* JSON parsing with `json-glib`
* WebSocket streaming via `libsoup-3.0`
* VSCode + Dev Container support for streamlined development

---

## Getting Started

### Prerequisites

* [Docker](https://www.docker.com/)
* [Visual Studio Code](https://code.visualstudio.com/)
* [Remote - Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)
* A valid [Deepgram API key](https://console.deepgram.com/signup)

### Step 1: Set Deepgram API Key on Host

Step 1: Create and Fill Your .env File

Create a .env file in the root of the project (you can copy from the example):

```bash
cp .env.example .env
```

Then edit .env and insert your Deepgram API key:

```
DEEPGRAM_API_KEY=your-token-here
```

This file is sourced during image build.

### Step 2: Open in Dev Container

From VSCode:

```text
File > Open Folder > gst-deepgram
```

Then reopen in container when prompted.

### Step 3: Build the Plugin

```bash
cmake -B build
cmake --build build
```

### Step 4: Run the Example App

```bash
GST_PLUGIN_PATH=build ./build/deepgram_test_app /home/vscode/test.wav
```

Output will show Deepgram's transcription results.

---

## GStreamer Usage

Run with `gst-launch-1.0`:

```bash
GST_PLUGIN_PATH=build \
  gst-launch-1.0 filesrc location=/home/vscode/test.wav ! \
  decodebin ! audioconvert ! audioresample ! deepgramsink deepgram-api-key="$DEEPGRAM_API_KEY"
```

Or inspect the plugin:

```bash
GST_PLUGIN_PATH=build gst-inspect-1.0 deepgramsink
```

---

## Development Notes

* Environment variable `DEEPGRAM_API_KEY` is required to run the plugin
* Sample audio (`test.wav`) is auto-downloaded in the dev container
* Modify `.vscode/launch.json` or `devcontainer.json` as needed

---

## License

MIT License

Copyright (c) 2025 Max Golovanchuk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
