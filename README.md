to compile run ```gcc -O2 -o oled_fbcp oled_fbcp.c```
and then ```sudo ./oled_fbcp```

to run at boot, pop the .service file into `/etc/systemd/system/` then hit `sudo systemctl daemon-reload` then `sudo systemctl enable oled.service` 

reboot to see changes.
