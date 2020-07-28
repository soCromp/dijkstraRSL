# Dockerfile to build skipvector developer image
# Doesn't currently build due to LLVM problems, so temporarily disabled.

FROM ubuntu:latest

MAINTAINER Matthew Rodriguez (mar316@lehigh.edu)

ENV TZ=America/New_York
ENV DEBIAN_FRONTEND="noninteractive"

# tzdata can be a build-killer, so install it... earlier
RUN apt-get update -y
RUN apt-get install -y --fix-missing tzdata 

# Grab wget, so that we can add the LLVM PPA
RUN apt-get install wget gnupg gnupg2 gnupg1 -y

# Add PPA for LLVM

RUN echo deb http://apt.llvm.org/focal/ llvm-toolchain-focal-10 main >> /etc/apt/sources.list
RUN echo deb-src http://apt.llvm.org/focal/ llvm-toolchain-focal-10 main >> /etc/apt/sources.list
RUN wget -qO - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -

# Upgrade / update the whole system, and pull from the new PPAs
RUN apt-get update -y
RUN apt-get upgrade -y

# Install additional software needed for development (you can add to this list)
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y gsl-bin

# Basic C++ development
RUN DEBIAN_FRONTEND=noninteractive apt install -y --fix-missing \
  build-essential \
  cmake \
  curl \
  dos2unix \
  emacs-nox \
  g++-8 \
  g++-multilib \
  gdb \
  gdbserver \
  git \
  gnuplot \
  man \
  perl \
  psmisc \
  screen \
  tar \
  unzip \
  valgrind \
  vim \
  wget \
  xz-utils

# Clang / LLVM
RUN DEBIAN_FRONTEND=noninteractive apt install -y \
  libllvm10 \
  llvm-10 \
  llvm-10-dev \ 
  llvm-10-doc \ 
  llvm-10-examples \ 
  llvm-10-runtime \ 
  clang-10 \ 
  libclang-common-10-dev \ 
  libclang-10-dev \ 
  libclang1-10 \ 
  clang-format-10 \ 
  clangd-10 \ 
  lldb-10 \ 
  lld-10 \ 
  libc++-10-dev \ 
  libc++abi-10-dev \ 
  libomp-10-dev

# Change the working directory:
WORKDIR /root

# To use this Dockerfile
# 1 - Make an image, let's say it's called "sv"
#     - Go to the folder where this Dockerfile exists
#     - On Linux: sudo docker build -t sv .
#     - On Win:   docker build -t sv .
# 2 - Copy code/mydock.sh to code/dock.sh (or .bat instead of .sh)
# 3 - Update code/mydock.sh to point to your checkout of this repository
# 4 - Run code/mydock.sh to start an interactive shell
