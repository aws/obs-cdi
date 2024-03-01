Build Instructions for OBS Studio CDI Plugin
---

These are the build instructions for the OBS Studio CDI plugin. The broad steps are to build CDI, build OBS Studio, then build the OBS Studio CDI plugin. You must build OBS Studio from source in order to have a development environment with all the needed headers. This plugin will enable CDI as an OBS Studio source and output.

**OBS Studio 30.0.2** is the latest version this plugin supports. Newer versions may require changes to the information in this guide and/or the plugin.

**CDI outputs**: For YCbCr outputs, OBS Studio's pixel format must be set to ```I444```. For RGB outputs, the pixel format must be set to ```BGRA (8-bit)```. Audio is supported from 1-8 channels, as limited by OBS Studio. We have tested this plugin with various frame rates and raster sizes but find that 1080p60 performs the best.

**CDI sources**: No additional configuration of OBS Studio is required. Up to 8 audio channels are supported, as limited by OBS Studio.

<!-- @import "[TOC]" {cmd="toc" depthFrom=1 depthTo=6 orderedList=false} -->

<!-- code_chunk_output -->

- [Installing on Windows](#installing-on-windows)
  - [Launch Windows EC2 Instance](#launch-windows-ec2-instance)
  - [Download and Build AWS CDI-SDK and Dependencies on Windows](#download-and-build-aws-cdi-sdk-and-dependencies-on-windows)
  - [Download and Build OBS Studio and Dependencies on Windows](#download-and-build-obs-studio-and-dependencies-on-windows)
    - [Download and Build the OBS Studio CDI Plugin on Windows](#download-and-build-the-obs-studio-cdi-plugin-on-windows)
- [Debuggging the OBS Studio CDI Plugin on Windows](#debuggging-the-obs-studio-cdi-plugin-on-windows)
- [Installing on Linux](#installing-on-linux)
  - [Configure Rocky Linux 9 for GUI](#configure-rocky-linux-9-for-gui)
  - [Install NVIDIA driver on Linux](#install-nvidia-driver-on-linux)
  - [Install NiceDCV for remote GUI access on Linux](#install-nicedcv-for-remote-gui-access-on-linux)
  - [Download and Build AWS CDI-SDK and Dependencies on Linux](#download-and-build-aws-cdi-sdk-and-dependencies-on-linux)
  - [Download and Build OBS Studio and Dependencies on Linux](#download-and-build-obs-studio-and-dependencies-on-linux)
    - [Install OBS Studio dependencies](#install-obs-studio-dependencies)
  - [Download and Build the OBS Studio CDI Plugin on Linux](#download-and-build-the-obs-studio-cdi-plugin-on-linux)
    - [Install plugin dependencies](#install-plugin-dependencies)
    - [Optional: install Visual Studio Code](#optional-install-visual-studio-code)
    - [Download plugin](#download-plugin)
    - [Build plugin](#build-plugin)
  - [Debugging the OBS Studio CDI Plugin on Linux](#debugging-the-obs-studio-cdi-plugin-on-linux)
- [CDI Output Configuration](#cdi-output-configuration)
  - [CDI Source Configuration](#cdi-source-configuration)
  - [OBS Studio CDI Plugin Logging](#obs-studio-cdi-plugin-logging)

<!-- /code_chunk_output -->

# Installing on Windows

## Launch Windows EC2 Instance

**Note**: These steps have been verified to work for both Debug and Release builds.

Launch an EC2 instance with EFA as the primary network adapter. A minimum of using a c5n.8xlarge or g4dn.8xlarge is currently required.

-   Use Windows Server 2019 base AMI
-   Use public IP
-   Use at least 100GB for EBS volume due to size of OBS/QT install

Configure security settings for Windows RDP and use RDP client to connect to the instance.

## Download and Build AWS CDI-SDK and Dependencies on Windows

Follow the instructions at [INStALL_GUIDE_WINDOWS.md](https://github.com/aws/aws-cdi-sdk/blob/mainline/INSTALL_GUIDE_WINDOWS.md) with the following changes:

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

## Download and Build OBS Studio and Dependencies on Windows

Follow the instructions at [Windows build directions](https://obsproject.com/wiki/install-instructions#windows-build-directions). A few additional notes are below:

When creating the OBS Studio Visual Studio project files, use a **Visual Studio Powershell** to run these commands:

 ```
 cd obs-studio
cmake --preset windows-x64
 ```

Build OBS Studio as a ```Debug``` or ```Release``` build: Build → Build Solution

**NOTE**: Not all sub-projects always build successfully, but the necessary ones do. Look at the output tab when building is done to verify this. Then, run OBS Studio which is located at ```build_x64/rundir/Debug/bin/64bit/obs64.exe``` or ```build_x64/rundir/Release/bin/64bit/obs64.exe``` to verify it built properly.

### Download and Build the OBS Studio CDI Plugin on Windows

In a **Visual Studio Powershell**, navigate to the location you would like the OBS CDI plugin to download to. Download the OBS Studio CDI plugin repository using:

```
git clone https://github.com/aws/obs-cdi.git
```

Then, use the commands shown below to generate the Visual Studio solution and project files. Set **CDI_DIR** to that path where the CDI-SDK was installed and set **CMAKE_INSTALL_PREFIX** to the path where the built OBS Studio was installed. Default paths are shown.

```
cd obs-cdi
mkdir build
cmake -B ./build --preset windows-x64 -DCDI_DIR="C:/CDI/aws-cdi-sdk" -DCMAKE_INSTALL_PREFIX="C:/obs-studio"
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

# Debuggging the OBS Studio CDI Plugin on Windows

Open the obs-cdi Visual Studio solution in Visual Studio. The default execution target is ```ALL_BUILD```.

In the Solution Explorer window, expand the ```CmakePredefinedTargets``` and right-click on the ```ALL_BUILD``` project. Select ```Properties```. On the ```ALL_BUILD``` Propety Pages window, select ```Configuration Properties``` and change the settings shown below to point to where you installed OBS (examples shown):

Debug Command:
 ```C:/obs-studio/build_x64/rundir/Debug/bin/64bit/obs64.exe```

Working Directory:
 ```C:/obs-studio/build_x64/rundir/Debug/bin/64bit```

You can now set breakpoints and launch OBS Studio, which will load the CDI plugin, from within Visual Studio.

# Installing on Linux

**Note**: These steps have been verified to work for both Debug and Release builds on **Rocky Linux 9**.

Launch an EC2 instance with EFA as the primary network adapter. A minimum of using a g4dn.8xlarge is currently required.

-   Use Rocky Linux 9 base AMI
-   Use public IP
-   Use at least 100GB for EBS volume due to size of OBS/QT install

## Configure Rocky Linux 9 for GUI

ssh to the instance using "rocky" as username and the key file used when the instance was created. Then run these steps:

```
sudo dnf upgrade -y
sudo dnf groupinstall "Server with GUI" -y
sudo systemctl set-default graphical

# Create a password for rocky user:
sudo passwd rocky
```

## Install NVIDIA driver on Linux

Steps shown below were create using [this guide](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/install-nvidia-driver.html).

**Install dependencies**

```
sudo dnf install -y make gcc kernel-devel kernel-headers elfutils-libelf-devel elfutils-devel libglvnd-devel.x86_64
sudo dnf config-manager --set-enabled crb
sudo dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm
sudo dnf install -y https://dl.fedoraproject.org/pub/epel/epel-next-release-latest-9.noarch.rpm
sudo dnf install -y wget java-17-openjdk java-17-openjdk-devel apr apr-util mesa-libGLU xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm
```

**Blacklist the Nouveau driver**
```
cat << EOF | sudo tee --append /etc/modprobe.d/blacklist-nouveau.conf
blacklist nouveau
options nouveau modeset=0
EOF

sudo touch /etc/modprobe.d/blacklist-nouveau.conf
sudo echo 'omit_drivers+=" nouveau "' | sudo tee --append /etc/dracut.conf.d/blacklist-nouveau.conf > /dev/null
sudo dracut --regenerate-all --force

# Enable console mode and reboot.
sudo systemctl set-default multi-user.target
sudo reboot
```

**Download and install the NVIDIA driver**

Install **aws cli** using [this guide](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html). Then download the latest driver and install using these commands:

```
aws s3 cp --recursive s3://ec2-linux-nvidia-drivers/latest/ .
chmod +x NVIDIA-Linux-x86_64*.run
sudo /bin/sh ./NVIDIA-Linux-x86_64*.run

# Enable graphical mode and configure NVIDIA.
sudo systemctl set-default graphical.target
```

Optionally, instead of installing **aws cli** you can use **wget** to download the driver. To find the filename of the latest driver use [this link]("https://s3.amazonaws.com/ec2-linux-nvidia-drivers/").

Then, look for **\<Key\>latest/**. An example is shown below that shows the key and the command used to download the driver:

```
<Key>latest/NVIDIA-Linux-x86_64-550.54.14-grid-aws.run</Key>

wget "s3://ec2-linux-nvidia-drivers/latest/NVIDIA-Linux-x86_64-550.54.14-grid-aws.run"
```

**Disable GSP**

```
sudo touch /etc/modprobe.d/nvidia.conf
sudo echo "options nvidia NVreg_EnableGpuFirmware=0" | sudo tee --append /etc/modprobe.d/nvidia.conf

sudo nvidia-persistenced
sudo nvidia-smi -ac 5001,1590
sudo reboot
```

## Install NiceDCV for remote GUI access on Linux

**Note**: Using Windows RDP is not recommended. I was not able to get to work correctly with OBS Studio's source rendering window (it would not render).

The install steps shown below were based on the [DCV Linux Install Guide](https://docs.aws.amazon.com/dcv/latest/adminguide/setting-up-installing-linux.html) to install NiceDCV for **console** sessions.

**Note**: Must enable S3 for dcv-license by adding permission to AMI associated with the instance. Details are [here](https://docs.aws.amazon.com/dcv/latest/adminguide/setting-up-license.html).

Update the security group settings to allow incoming TCP traffic on port 8443.

**Configure firewall**

Can either disable the firewall or allow TCP traffic on port 8443.

```
# Disable firewall:
sudo setenforce 0
sudo sed -i 's/^SELINUX=.*/SELINUX=disabled/g' /etc/sysconfig/selinux
sudo sed -i 's/^SELINUX=.*/SELINUX=disabled/g' /etc/selinux/config
sudo systemctl stop firewalld
sudo systemctl disable firewalld

# Or allow TCP traffic on port 8443:
sudo firewall-cmd --permanent --add-port=3389/tcp
sudo firewall-cmd --reload
```

**Configure X Server**
```
sudo systemctl set-default graphical.target
sudo yum install glx-utils
sudo nvidia-xconfig --preserve-busid --enable-all-gpus
sudo rm -rf /etc/X11/XF86Config*
sudo systemctl isolate multi-user.target
sudo systemctl isolate graphical.target
```

**Install the NiceDCV server**

```
sudo rpm --import https://d1uj6qtbmh3dt5.cloudfront.net/NICE-GPG-KEY
wget https://d1uj6qtbmh3dt5.cloudfront.net/nice-dcv-el9-x86_64.tgz
tar -xvzf nice-dcv-el9-x86_64.tgz
cd nice-dcv-2023.1-16388-el9-x86_64
sudo yum install nice-dcv-server*.rpm
sudo systemctl isolate multi-user.target
sudo systemctl isolate graphical.target
```

**Start the NiceDCV server**
```
sudo systemctl start dcvserver
sudo systemctl enable dcvserver
```

**Create a DCV console session**

Use the **rocky** user to create a session either [automatically on startup](https://docs.aws.amazon.com/dcv/latest/adminguide/managing-sessions-start.html#managing-sessions-start-auto) or manually using the steps below:

```
sudo dcv create-session console --type console --owner rocky
dcv list-sessions
```

**Connect from client**

After all the steps above were completed, I had to then re-run these commands (once) on the instance for the connection to work:
```
sudo systemctl isolate multi-user.target
sudo systemctl isolate graphical.target
```

Install a NiceDCV client on your client system and then, use the connection string below to connect to the instance, replacing **remote_ip** with the IP address of your instance:
```
<remote_ip>:8443?transport=auto#console
```

**Note**: When prompted, use "rocky" as the user. For second part of login, can select **Other User** and use the "rocky" user's password.

## Download and Build AWS CDI-SDK and Dependencies on Linux

Follow the instructions at [INSTALL_GUIDE_LINUX.md](https://github.com/aws/aws-cdi-sdk/blob/mainline/INSTALL_GUIDE_LINUX.md) with the following changes:

Skip the **Install AWS CloudWatch and AWS CLI** section.

For the **Build CDI libraries and test applications** section, replace the build steps with the commands shown below. This will build a debug variant with CloudWatch metrics gathering disabled.

```
cd aws-cdi-sdk/
make DEBUG=y NO_MONITORING=y
```

## Download and Build OBS Studio and Dependencies on Linux

The steps below for **Rocky Linux 9** were created using the instructions at [Red Hat-based directions](https://github.com/obsproject/obs-studio/wiki/build-instructions-for-linux#red-hat-based).


### Install OBS Studio dependencies

```
# Install rpm fusion
sudo dnf install --nogpgcheck https://dl.fedoraproject.org/pub/epel/epel-release-latest-$(rpm -E %rhel).noarch.rpm
sudo dnf install --nogpgcheck https://mirrors.rpmfusion.org/free/el/rpmfusion-free-release-$(rpm -E %rhel).noarch.rpm
sudo dnf install --nogpgcheck https://mirrors.rpmfusion.org/nonfree/el/rpmfusion-nonfree-release-$(rpm -E %rhel).noarch.rpm

# Install dev tools
sudo dnf groupinstall "Development Tools" -y
sudo dnf install alsa-lib-devel asio-devel cmake ffmpeg-free-devel fontconfig-devel freetype-devel gcc gcc-c++ gcc-objc git glib2-devel json-devel libavcodec-free-devel libavdevice-free-devel libcurl-devel libdrm-devel libglvnd-devel -y

# This package was not found, so used RPM.
# sudo dnf install libjansson-devel
wget https://dl.rockylinux.org/pub/rocky/9/devel/x86_64/os/Packages/j/jansson-devel-2.14-1.el9.x86_64.rpm
sudo dnf install jansson-devel-2.14-1.el9.x86_64.rpm -y

sudo dnf install libuuid-devel libva-devel libv4l-devel libX11-devel libXcomposite-devel libXinerama-devel luajit-devel mbedtls-devel pciutils-devel pciutils-devel pipewire-devel pulseaudio-libs-devel python3-devel -y
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtsvg-devel qt6-qtwayland-devel -y

# This package was not found and I could not find a RPM. Doesn't seem to be required.
# sudo dnf install qt6-qtx11extras-devel -y

sudo dnf install speexdsp-devel swig systemd-devel vlc-devel wayland-devel websocketpp-devel x264-devel -y

# Additional packages that I had to install.
sudo dnf install libxkbcommon-devel libqrcodegencpp-devel oneVPL-devel srt-devel librist-devel -y
```

**Upgrade from default cmake is required**

Had to upgrade to a newer version of cmake. Default version is 3.20, while OBS Studio requires 3.22 or later.

```
wget https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3.tar.gz
tar -xvf cmake*.gz
cd cmake-3.28.3
./bootstrap
make -j10
sudo make install -j10

# Add this to ~/.bashrc so the new cmake is used by default.
export PATH="/usr/local/bin:$PATH"
```

**Download and build OBS Studio**
OBS Studio version 30.0.2 was tested. Newer versions may require addtional changes to this document and files. Download the files and checkout version 30.0.2 using:

```
git clone --recursive https://github.com/obsproject/obs-studio.git
cd obs-studio
git checkout -b 30.0.2 30.0.2
```

To build, modify the **CMAKE_INSTALL_PREFIX** path shown below as desired. **Note**: You must use the same path when building the OBS Studio CDI plugin.

```
mkdir build && cd build
cmake -DLINUX_PORTABLE=ON -DCMAKE_INSTALL_PREFIX="${HOME}/obs-studio-portable" -DENABLE_BROWSER=OFF -DENABLE_AJA=OFF -DENABLE_WEBRTC=OFF -DENABLE_WEBSOCKET=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

Ensure OBS Studio runs before building and installing the OBS Studio CDI plugin. The default path is shown in the example below:
```
cd ~/obs-studio-portable/bin/64bit
./obs
```


## Download and Build the OBS Studio CDI Plugin on Linux

### Install plugin dependencies

```
sudo dnf install ninja-build
```

### Optional: install Visual Studio Code

If you prefer to use Visual Studio Code to build and debug the plugin, you can install it using [this guide](https://code.visualstudio.com/docs/setup/linux). A Visual Studio Code workspace file **obs-cdi.code-workspace** and associated **.vscode** folder are included with the plugin.

### Download plugin

In a terminal, navigate to the location you would like the OBS CDI plugin to download to. Download the OBS Studio CDI plugin repository using:

```
git clone https://github.com/aws/obs-cdi.git
```

### Build plugin

To generate the makefiles used to build a Debug variant of the plugin, use the commands shown below. Set **CDI_DIR** to that path where the CDI-SDK was installed and set **CMAKE_INSTALL_PREFIX** to the path where the built OBS Studio was installed. Default paths are shown.

```
mkdir build
cmake -B ./build --preset linux-x86_64 -DCMAKE_BUILD_TYPE=Debug -DCDI_DIR="/home/rocky/CDI/aws-cdi-sdk" -DCMAKE_INSTALL_PREFIX="/home/rocky/obs-studio-portable"
```

If successful, output should look something like:

```
-- Build files have been written to: /home/rocky/obs-cdi/build
```

Now, build the plugin and install in OBS Studio using:

```
cd build
ninja
ninja install
```

**Note**: You can also use Visual Code to perform these steps. Open the **obs-cdi.code-workspace** Visual Code workspace file and set **CDI_DIR** and **CMAKE_INSTALL_PREFIX** using **Terminal->Configure Task**. This will open the **launch.json** file so you can edit it.

## Debugging the OBS Studio CDI Plugin on Linux

Open the **obs-cdi.code-workspace** Visual Code workspace file in Visual Code.

Select **Run->Open Configurations**. The **launch.json** file will be displayed in the editor. For **"program"**, set the full path to the obs binary and set **"cwd"** to the obs folder. For example:

```
    "program": "/home/rocky/obs-studio-portable/bin/64bit/obs",
    ...
    "cwd": "/home/rocky/obs-studio-portable/bin/64bit",
```

You can now set breakpoints and launch OBS Studio, which will load the CDI plugin, from within Visual Code.

# CDI Output Configuration

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

Use the CDI Source Properties to set the following configuration settings:

```
Local EFA Adapter IP - The local IP address assigned to the EFA adapter
Local Bind IP - If using a single adapter, leave blank. Otherwise  the IP address of the adapter to bind to
Port - The port to listen to for the CDI connection
Enable Audio - Check to enable audio (default is enabled)
```

## OBS Studio CDI Plugin Logging

**NOTE**: This plugin will generate log messages in the default OBS log folder located in Windows at ```C:\Users\<username>\AppData\Roaming\OBS\logs``` and Linux at ```~/.config/obs-studio/logs```. This log file can get very large if there is not a valid CDI target to connect to.  The log will fill with messages about trying to connect, so it is recommended your CDI source and/or receiver is setup before selecting the OBS Studio CDI source or enabling the OBS Studio CDI output.
