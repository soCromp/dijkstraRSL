#!/bin/sh
echo "dock.sh:"
echo "  Launch an interactive development container"
echo "Prerequisites:"
echo "  Docker"
echo "Notes:"
echo "  You should copy this to mydock.sh and make changes to it there."
echo "  You may also want to move it up one directory for convenient access."
echo "  See the comments in this file for more details."
echo "  On Windows, use dock.bat instead."

# HOME is the folder in the local filesystem that will be mapped to /root in the
# container.  This should be the fully-qualified path to your checkout of the 
# cds-iteration repository
#HOME=/c/Users/username/path/skipvector

# IMAGE is the name of the image to run.  See the Dockerfile for more info
#IMAGE=sv

# Now that everything is configured, go ahead and launch a container
docker run --privileged -v /Users/snc/Box\ Sync/Skola/Lehigh/SprayList:/root --rm -it sl
