# Full-Screen Wrapper for DLSS5

### A demo app that applies DLSS5 to the entire screen (even the desktop) with full model controls.

Just a single `exe` (signed with a trusted certificate) written in C++ with zero third-party dependencies.

<br>

> [!NOTE]
> This is an UNOFFICIAL project, not ssociated with Nvidia.

# App Screenshot
<p align="center">
<img width="750" src="https://github.com/user-attachments/assets/0db29600-2e94-4714-9c36-5dbe2a446619" />
</p>

## How to Download and Use
1. Download the latest version of the `exe`. (Direct link [here](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/releases/latest/download/FullScreenWrapperForDLSS5.exe))
    - Or find it under the latest [Release](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/releases) under "Assets"
2. Acquire `nvngx_dlssnr.dll` and put it next to the `exe` (see [Requirements](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper#requirements) section below)
3. Run the `exe` (no installation required).
4. By default, it applies to the primary monitor, with more options in the View tab. You can also apply it to a specific selected window.

## Key Features:
- It's a single `.exe` file, no install or third party dependencies required (Besides the official DLSS5 dll: `nvngx_dlssnr.dll`)
    - It's also signed with a trusted certificate.
- Does NOT inject itself into or modify any other applications. It reads the final screen output and processes that. (See [How It Works](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper#how-it-works) explanation below)
  - Therefore it should present no more false-positive anti-cheat risk than ordinary screen-capture software or graphics-enhancement overlay software.
- Full control over internal model inputs. Including uncapped values for structure and tone.
- Ability to selectively apply it to a specific window.

# Example Screenshots

> [!IMPORTANT]
> The output of this app will look different than if used with a natively supported DLSS5 game.
> 
> Native DLSS5 Games can provide the model with much more data such as motion data, object depth info, exact skin maps, etc to create a better result.
>
> This also means the performance/fps output from this app is NOT what the performance would be from a natively implemented game.
> 
> The output also obviously depends on the settings, which can be set to extreme values using this app, but would never actually be used in a real game.


<h3 align="center">Original Screenshot:</h3>
<p align="center">
<img width="2551" height="1587" alt="Oblivion Original" src="https://github.com/user-attachments/assets/c75de803-3a08-4025-9d76-cfcaa7922996" />
</p>

<h3 align="center">Standard Tone and Structure Settings:</h3>
<p align="center">
<img width="2552" height="1589" alt="Oblivion Standard" src="https://github.com/user-attachments/assets/8af48486-0b97-4d8a-a5f7-8b2b930f9006" />
</p>

<h3 align="center">3X Multiplier:</h3>
<p align="center">
<img width="2555" height="1589" alt="Oblivion 3x Standard" src="https://github.com/user-attachments/assets/7f6b574f-2143-4d25-bc00-032c2d24f38a" />
</p>

<h3 align="center">20X Multiplier:</h3>
<p align="center">
<img width="2554" height="1585" alt="Oblivion 20x" src="https://github.com/user-attachments/assets/9f2422c9-cc0c-4bff-81cc-02a0c210882b" />
</p>

------


# Requirements

- You need a 50-Series Nvidia GPU and Nvidia drivers `616.64` or newer
- You must acquire `nvngx_dlssnr.dll` yourself and put it next to the app `exe`.
  - Just Google it, you can find people who have uploaded it like on reddit.
  - For copyright reasons I will not host or link to it from here.
  - Note: The app will verify the dll's signature to ensure it's the right file either way.

### Optional:
- `nvngx_dlss.dll` - Enables use of super resolution options. Also put that next to the `exe`.

------

# How It Works
On a simple level, it:
1. Captures the regular screen output (or a screen region) using the Windows API
2. Passes the video stream into the DLSS5 model (which is in the `dll` file), along with chosen processing options.
3. Receives the new processed video data
4. Creates a brand new borderless window that is shown on top.
    - In other words, it doesn't directly modify the other apps themselves. They still are technically showing their original windows underneath it. Almost as if you put a video camera recording the screen, which applies effects then outputs to a second monitor. Except in this case it's a new window.
    - This window is "click through", so it is effectively invisible to the cursor. This means you can click, hover, and interact with everything beneath just as you normally would.
    - If you have it set to affect only a specific window, it only covers that window. If set to apply to the whole screen, the window covers the entire screen. 

For a much more detailed and technical explanation, see the [Advanced Readme](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/blob/main/Readme_Advanced.md) file  (`Readme_Advanced.md`).
