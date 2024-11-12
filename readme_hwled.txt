kxlu/WLED is forked from WLED

Repo: github.com/kxlu/WLED

The main branch is the default branch and sync to the upstream repo
the hwled branch is the branch to make custom code changes

Version 1 : "HOPE"

1. Forked on 2024-11-11 from WLED/main

2. Added code to set boot preset at JSON API. Post submit to settings/leds to update BP will not save to FS. 
  at set.cpp handleSettings() Line 720 doSerializeConfig is False when SUBPAGE is LED settings. 
  Therefore 'pboot' is added to JSON API.

    json.cpp deserialState() #434-435

3. Added code to return "hwld" version name "HOPE"  on Info object to identify this is a custom build

    json.cpp serializeInfo()

4. Changes made on /hwld branch

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
