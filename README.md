Build Instructions for OBS Studio CDI Plugin
---

These are the build instructions for the OBS Studio CDI plugin. The broad steps are to build CDI, build OBS Studio, then build the OBS Studio CDI plugin. You must build OBS Studio from source in order to have a development environment with all the needed headers. This plugin will enable CDI as an OBS Studio source and output.

**OBS Studio 30.0.2** is the latest version this plugin supports. Newer versions may require changes to the information in this guide and/or the plugin.

**CDI outputs**: For YCbCr outputs, OBS Studio's pixel format must be set to ```I444```. For RGB outputs, the pixel format must be set to ```BGRA (8-bit)```. Audio is supported from 1-8 channels, as limited by OBS Studio. We have tested this plugin with various frame rates and raster sizes but find that 1080p60 performs the best.

**CDI sources**: No additional configuration of OBS Studio is required. Up to 8 audio channels are supported, as limited by OBS Studio. 

<!-- @import "[TOC]" {cmd="toc" depthFrom=1 depthTo=6 orderedList=false} -->

<!-- code_chunk_output -->

- [Launch EC2 Instance](#launch-ec2-instance)
  - [Download and Build AWS CDI-SDK and Dependencies](#download-and-build-aws-cdi-sdk-and-dependencies)
  - [Download and Build OBS Studio and Dependencies](#download-and-build-obs-studio-and-dependencies)
    - [Download and Build the OBS Studio CDI Plugin](#download-and-build-the-obs-studio-cdi-plugin)
- [Debuggging the OBS Studio CDI Plugin](#debuggging-the-obs-studio-cdi-plugin)
  - [CDI Output Configuration](#cdi-output-configuration)
  - [CDI Source Configuration](#cdi-source-configuration)
  - [OBS Studio CDI Plugin Logging](#obs-studio-cdi-plugin-logging)

<!-- /code_chunk_output -->

# Launch EC2 Instance

 **Note**: These steps have only been verified to work for both Debug and Release builds

Launch an EC2 instance with EFA as the primary network adapter. A minimum of using a c5n.8xlarge or g4dn.8xlarge is currently required.

-   Using Windows Server 2019 base AMI
-   Use public IP
-   Use at least 100GB for EBS volume due to size of OBS/QT install

## Download and Build AWS CDI-SDK and Dependencies

Follow the instructions at [https://github.com/aws/aws-cdi-sdk/blob/mainline/INSTALL\_GUIDE\_WINDOWS.md](https://github.com/aws/aws-cdi-sdk/blob/mainline/INSTALL_GUIDE_WINDOWS.md) with the following changes:

Install Microsoft Visual Studio 2022 instead of 2019. To install use Chocolatey from Powershell using:

```choco install visualstudio2022-workload-nativedesktop -y```

After you have downloaded the aws-cdi-sdk, and before running the ```install.ps1``` script, disable the CloudWatch metrics in ```aws-cdi-sdk/src/cdi/configuration.h``` by commenting out lines for defining ```CLOUDWATCH_METRICS_ENABLED``` and `METRICS_GATHER_SERVICE_ENABLED`. After commenting out, those lines should look like:

```
//#define CLOUDWATCH_METRICS_ENABLED
//#define METRICS_GATHERING_SERVICE_ENABLED
```

Then, follow the remaining steps to build and install the AWS CDI-SDK using ```install.ps1```. The script will download dependencies, install the EFA driver and build the CDI-SDK.

The OBS Studio CDI plugin requires the ```Debug_DLL``` or ```Release_DLL``` variant of the AWS CDI-SDK.

To build it, Use Visual Studio to open the ```aws-cdi-sdk/proj/cdi_proj.sln``` Visual Studio solution file. For build type select either ```Debug_DLL``` or ```Release_DLL``` from the dropdown and then use Build → Build Solution to build it.

## Download and Build OBS Studio and Dependencies

Follow the instructions at [https://obsproject.com/wiki/install-instructions\#windows-build-directions](https://obsproject.com/wiki/install-instructions#windows-build-directions). A few additional notes are below:

When creating the OBS Studio Visual Studio project files, use a Visual Studio Powershell to run these commands:

 ```
 cd obs-studio
cmake --preset windows-x64
 ```

Build OBS Studio as a ```Debug``` or ```Release``` build: Build → Build Solution

**NOTE**: Not all sub-projects always build successfully, but the necessary ones do. Look at the output tab when building is done to verify this. Then, run OBS Studio which is located at ```build_x64/rundir/Debug/bin/64bit/obs64.exe``` or ```build_x64/rundir/Release/bin/64bit/obs64.exe``` to verify it built properly.

### Download and Build the OBS Studio CDI Plugin

In a Visual Studio Powershell, navigate to the location you would like the OBS CDI plugin to download to. Download the OBS Studio CDI plugin repository using:

```
git clone https://github.com/aws/obs-cdi.git
```

Modify the configuration settings in ```obs-cdi/CMakeLists.txt``` located at the top of the file, as shown below:

```
# --------------------------------------------------------------------------------
# Configure these settings a necessary. Example paths are shown.
# --------------------------------------------------------------------------------
# Path to CDI-SDK root folder:
set(CDI_DIR "C:/CDI/aws-cdi-sdk")

# Path where OBS Studio was installed and built using Debug build:
set (OBS_DIR "C:/obs-studio")
```

In a Visual Studio Powershell, navigate to where you installed the plugin and use these commands:

```
cd obs-cdi
mkdir build
cmake -B ./build --preset windows-x64
```

If successful, output should look something like:

```
-- Build files have been written to: C:/obs-cdi/build
```

Using Visual Studio, open the solution file ```obs-cdi.sln``` that was generated in the build folder.

Build the plugin as a ```Debug``` or ```Release``` build: Build → Build Solution

**Note: For performance reasons, the Debug variant is not able to support 1080p@60 with bit depths greater than 8-bits. However, the Release variant does support all bit depths up to and including 1080p@60.**

Note: The project includes a post build script that copies all the necessary files into the right places in the OBS Studio rundir.

In Windows firewall allowed applications, allow the ```obs64.exe``` executable.

# Debuggging the OBS Studio CDI Plugin

Open the obs-cdi Visual Studio solution in Visual Studio. The default execution target is ```ALL_BUILD```.

In the Solution Explorer window, expand the ```CmakePredefinedTargets``` and right-click on the ```ALL_BUILD``` project. Select ```Properties```. On the ```ALL_BUILD``` Propety Pages window, select ```Configuration Properties``` and change the settings shown below to point to where you installed OBS (examples shown):

Debug Command:
 ```C:/obs-studio/build_x64/rundir/Debug/bin/64bit/obs64.exe```

Working Directory:
 ```C:/obs-studio/build_x64/rundir/Debug/bin/64bit```

You can now set breakpoints and launch OBS Studio, which will load the CDI plugin, from within Visual Studio.

## CDI Output Configuration

Before turning on the CDI output, make sure the OBS Studio video and audio settings are compatible with the plugin.

Settings → Video
```
Base (Canvas) Resolution → 1920x1080
Output (Scaled) Resolution → 1920x1080
FPS → 60
```

Settings → Advanced → Video
```
Color Format → "I444" for YCbCr output or "BGRA (8-bit)" for RGB output
Color Space → 709
Color Range → Full
```

Settings → Audio
```
Sample Rate → 48khz
Channels → Default is Stereo, but supports all the channel formats
```

To configure your CDI settings, use Menu → Tools → AWS CDI Output Settings

```
Main Output Name - Name for the output - defaults to “OBS”
Destination IP - the IP address of your CDI receiver
Destination Port - the destination port of your CDI receiver
Local EFA Adapter IP - your local IP address assigned to the EFA adapter 
Video Stream ID - CDI video stream identifier (0-65535). Default is 1.
Audio Stream ID - CDI audio stream identifier (0-65535). Default is 2.
Video Sampling - YCbCr 4:2:2, 4:4:4 and RGB.
RGB Alpha Used - Only available for RGB output.
Bit Depth - 8, 10 and 12-bit.
```

## CDI Source Configuration

Use the CDI Source Properties to setting the following configuratin settings:

```
Local EFA Adapter IP - your local IP address assigned to the EFA adapter
Listening Port - The port to listen to for the CDI connection
Enable Audio - Check to enable audio (default is enabled)
```

## OBS Studio CDI Plugin Logging

**NOTE**: This plugin will generate log messages in the default OBS log folder located at ```C:\Users\<username>\AppData\Roaming\OBS\logs```. This log file can get very large if there is not a valid CDI target to connect to.  The log will fill with messages about trying to connect, so it is recommended your CDI source and/or receiver is setup before selecting the OBS Studio CDI source or enabling the OBS Studio CDI output.
