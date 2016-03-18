The SPIFFS configuration is 4k sectors (the only size supported by the SDK) and 8k blocks. 256 byte pages. Magic is enabled and magic_len is also enabled. This allows the firmware to find the start of the filesystem (and also the size).
One of the goals is to make the filsystem more persistent across reflashing of the firmware.

There are two significant sizes of flash -- the 512K and 4M (or bigger). 

The file system has to start on a 4k boundary, but since it ends on a much bigger boundary, it also starts on an 8k boundary. For the small flash chip, there is 
not much spare space, so a newly formatted file system will start as low as possible (to get as much space as possible). For the large flash, the 
file system will start on a 64k boundary. A newly formatted file system will start between 64k and 128k from the end of the firmware. This means that the file 
system will survive lots of reflashing and at least 64k of firmware growth. 

The spiffsimg tool can also be built (from source) in the nodemcu-firmware build tree. 
