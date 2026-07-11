#!/bin/bash
set -e
cd "$(dirname "$0")"
curl -L -o deadpool.zip "https://archive.org/download/Deadpool-LivBs/Deadpool.zip"
echo "Done: deadpool.zip"
