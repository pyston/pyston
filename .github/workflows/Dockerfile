FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# set up locale
RUN apt-get update
RUN apt-get -y install locales locales-all
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# copied from https://code.visualstudio.com/remote/advancedcontainers/add-nonroot-user
ARG USERNAME=nonroot
ARG USER_UID=1000
ARG USER_GID=$USER_UID

# Create the user
RUN groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME \
    #
    # [Optional] Add sudo support. Omit if you don't need to install software after connecting.
    && apt-get update \
    && apt-get install -y sudo \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME

# ********************************************************
# * Anything else you want to do like clean up goes here *
# ********************************************************

# [Optional] Set the default user. Omit if you want to keep the default as root.
USER $USERNAME
