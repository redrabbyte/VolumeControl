### VolumeControl  


Control the Windows Play/Pause Media Key with just the volume control on a headset.

Some headsets only have one control - volume.  
This project supports using an up/down pattern on the volume control to trigger the Windows Play/Pause Media key.  
For this to work, the headset's volume control needs to control the *Windows* volume, not just the volume of headset.  


#### Functions & Settings


VolumeControl triggers a Play/Pause key press when the Windows volume is quickly changed first up, then down.  
For a short time after the detection, any volume changes are ignored, and the volume is reset to what it was before triggering the Play/Pause key press.  
The application will minimize to the system tray when closed via the X button, and can launch minimized and be started with Windows.  


*Max Delay* specifies how quickly you need to changed the volume to detect the pattern.  
*Blind Spot* specifies how long after a detection volume changes are ignored.  
*Min Volume Steps* specifies how much the volume needs to change to be detected.  
