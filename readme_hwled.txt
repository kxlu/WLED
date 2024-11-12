kxlu/WLED is forked from WLED

Repo: github.com/kxlu/WLED

The main branch is the default branch and sync to the upstream repo
the hwled branch is the branch to make custom code changes

== Version : "HOPE" 0.1 (2024-11-11)

1. Forked on 2024-11-11 from WLED/main

2. Changes made on /hwld branch

3. Files have been changed

    cfg.cpp
    const.h
    fcn_declare.h
    json.cpp
    set.cpp
    wled_server.cpp
    wled.h

4. Code Changes searched by #tags

#hwled      : A name to idenfity this is a custom build
#hwver      : hwled version number

#hwpboot    : Set boot preset

            We need an API to set boot preset by Python script.
            But ost submit to settings/leds for 'BP' does not write to FS.
            doSerializeConfig is False when subPage is LED settings
            Therefore we use JSON API state object to set boot preset

#hwlogin    : Add login security

            The current PIN validation is not enough when controllers running
            on Local network that can be accessed by others
             No login is required by default.
             No login is required on AP mode.
            On WiFi or Ethernet, use AP passcode as password to login.
            Use setting PIN Post Submit to enable/disable login requirement.
            Timeout after being inactive for 10 minutes, then relogin is required

#lc3200     : Board specific constants
            Define status LED IO 12
            Define Ethernet type WT32 so it does not need connect to AP to 
            set up Etherent. When Etherent cable is plugged in, Etherenet will be 
            connected right after restart from firmware installation.

5. Binary

    hwled_hope_0.1_esp32_eth.bin For general ESP32 Ethernt Boards
    hwled_hope_0.1_lc3200.bin For LC3200 Pixel Controller (Status LED, Ethernet Type)

    On Mac, install and use esptool to write firmware to the Board

    esptool [--port /dev/tty/.usbserial-...] erase_flash 
    esptool [--port /dev/tty/.usbserial-...] write_flash 0x0 esp32_bootloader_v4.bin 
    esptool [--port /dev/tty/.usbserial-...] erase_flash 0x0 hwled_hope_0.1_esp32_eth.bin

6. Default AP is WLED-AP (if Ethernet is not connected), default password is wled1234.

7. Go to  https://wled-compile.github.io/ to generate custom build. Add the result to the end
    of platform.io file. On PIO terminal (Not the one from VS Code), Enter
        pio run -e custom_build
    To open a PIO terminal, look at the bottom of VS Code, find the '>' icon and click it.

== Compile WLED on GitHub

To make a custom build workflow on a branch (other than the main branch), i.e. the hwled branch

- Go to https://wled-compile.github.io/ to generate custom build

- On 'Select WLED source' dropbox, choose 'Your own Fork/Repository'

- Enter GitHub account name and repository

- Make sure select the right branch from your own reposity if other than main branch

- Select your target and options

- After you are done, click the 'Download YAML Script to compile WLED on GitHub' button to download the yaml file to your computer

Now, you need to go to your forked reposity to create a workflow

- On the main branch (it seems you don't need to go to the branch you are going to compile), choose Actions, after you click Actions, right under it, there is a 'new workflow' button, click it

- At the page 'Choose a workflow', two lines below the title, click the link 'set up a workflow yourself'

- When the editor pop up, a default name 'main.yml' is shown. Enter the name 'wled_compile.yml'

- Paste the content from the YAML file generated

- Commit the changes. the wled_compile.yml is actually on the main branch (at the .github/workflows folder)

- Click 'Actions' menu again, you will see WLED compile on the left column

- Click and run the WLED compile worklfow. After success, a custon_build.bin is saved to your main branch
