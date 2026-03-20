# DiscordVoicePatcher

## Overview
DiscordVoicePatcher is a native Node.js addon designed to patch or modify Discord's voice functionality. It is implemented in C++ and can be built as a Node.js native module for use in Electron or Node.js environments.

## Features
- Native C++ patcher for Discord voice components
- Designed for integration with Electron-based Discord clients
- Customizable via `patcher.ini` configuration file
- Easy to rebuild for different Electron/Node.js versions

## Prerequisites
- **Node.js** (recommended: latest LTS)
- **node-gyp** (install globally with `npm install -g node-gyp`)
- **Python 3.x** (required by node-gyp)
- **Visual Studio Build Tools** (Windows) or Xcode (macOS)

## Building the Project
1. **Install dependencies:**
   - Make sure you have Node.js, Python, and Visual Studio Build Tools installed.
   - Install `node-gyp` globally if you haven't:
     ```sh
     npm install -g node-gyp
     ```
2. **Configure the build:**
   - If you need to target a specific Electron version, use:
     ```sh
     node-gyp rebuild --target=<electron_version> --arch=x64 --dist-url=https://electronjs.org/headers
     ```
     Example for (Discord) Electron 37.6.0:
     ```sh
     node-gyp rebuild --target=37.6.0 --arch=x64 --dist-url=https://electronjs.org/headers
     ```
   - For standard Node.js builds, simply run:
     ```sh
     node-gyp rebuild
     ```
3. **Result:**
   - The compiled module will be located in `build/Release/patcher.node`.

## Configuration
- Edit `patcher.ini` to customize patching behavior.
- Refer to comments in the file for available options.

## Usage

This module is primarily used by the [voicePatcher plugin](https://github.com/Loukious/Vencord/tree/main/src/plugins/voicePatcher.desktop) in the [Vencord](https://github.com/Loukious/Vencord) project. It is not intended for standalone use, but rather as a native dependency for the plugin.

If you are a user of Vencord, you do not need to interact with this module directly—installation and usage are handled automatically by the voicePatcher plugin.

If you are developing or debugging the plugin, you can load the compiled `patcher.node` in your Electron or Node.js project using `require()`:

```js
const patcher = require('./build/Release/patcher.node');
// Use patcher as needed
```

## Troubleshooting
- Ensure all prerequisites are installed and available in your PATH.
- If you encounter build errors, check that your Electron/Node.js version matches the build target.
- For issues with native modules, try cleaning the build:
  ```sh
  node-gyp clean
  node-gyp rebuild
  ```

## License
This project is provided as-is. See LICENSE file if available.
