# rp6502-emu Android Port

This subdirectory contains the Android wrapper, build files, and assets to compile and run the `rp6502-emu` emulator natively on Android devices (specially optimized for handheld consoles like the Retroid Pocket 3+).

---

## Prerequisites

Before building, ensure you have the following installed on your machine:
1. **Java JDK 17** (e.g., installable via Homebrew: `brew install openjdk@17`).
2. **Android SDK & NDK**:
   - Can be installed via Android Studio.
   - Recommended NDK version: **25.1.8937393** (configured automatically in `build.gradle`).
3. **ADB (Android Debug Bridge)**:
   - Part of the Android SDK platform tools, used to install APKs and push files.

---

## How to Build

All builds can be run directly from the command line using the Gradle wrapper inside this directory.

### Step 1: Navigate to the Android project folder
```bash
cd src/emu/android
```

### Step 2: Compile the APK
* **Debug Build** (non-optimized, contains debugger symbols):
  ```bash
  ./gradlew assembleDebug
  ```
  Generates: `build/outputs/apk/debug/rp6502-emu-debug.apk`

* **Release Build** (fully optimized with `-O3`, runs at full speed):
  ```bash
  ./gradlew assembleRelease
  ```
  Generates: `build/outputs/apk/release/rp6502-emu-release.apk`
  *(Note: The release build is configured in `build.gradle` to be signed with your local debug key, making it directly deployable via ADB for development).*

---

## Deploying to Device

1. Connect your Android device via USB and ensure **USB Debugging** is turned ON in Developer Options.
2. Verify the device is connected:
   ```bash
   adb devices
   ```
3. Install the optimized Release APK:
   ```bash
   adb install -r build/outputs/apk/release/rp6502-emu-release.apk
   ```

---

## Storage & ROM Setup

The emulator is designed to look for `.rp6502` ROMs in a public shared directory. It prioritizes storage in the following order:
1. **External Physical SD Card**: `/storage/YOUR_SD_CARD_ID/Download/rp6502/` (or `/storage/YOUR_SD_CARD_ID/rp6502/`)
2. **Internal Shared Storage**: `/sdcard/Download/rp6502/`
3. **App Sandbox Fallback**: `/data/data/com.picocomputer.rp6502/files/`

### 1. Granting Permission
Because Google restricts file access on Android 11+, the emulator requires the **All Files Access** (`MANAGE_EXTERNAL_STORAGE`) permission to read your ROM folders. 

* The app will automatically prompt you to grant this access on startup if it detects no ROM files.
* Pressing **SELECT**, **START**, or **HOME** in the empty menu will automatically open the Android system settings screen where you can enable **"Allow access to manage all files"** for the **rp6502 Emulator**.

### 2. Copying ROMs
Create the `rp6502` directory inside the public `Download` folder on either your internal storage or physical micro SD card, and copy your `.rp6502` ROMs into it.

**Example via ADB (using the public internal Downloads directory):**
```bash
adb shell mkdir -p /sdcard/Download/rp6502
adb push game.rp6502 /sdcard/Download/rp6502/game.rp6502
```

---

## Gamepad Controls & Menu Navigation

The emulator uses the physical controller mappings for the Retroid Pocket 3+:

* **ROM Selection Menu**:
  * **DPAD Up / Down** (or **Left Stick Up / Down**): Navigate the ROM list.
  * **A Button**: Boot the selected ROM.
  * **SELECT / START / HOME**: Triggers the settings redirect permission page (if access is not yet allowed) or refreshes the file list.
* **While Playing**:
  * Press **SELECT + START** (or the standalone **HOME / MODE** button) simultaneously to return to the ROM selection menu at any time to hot-swap games.
