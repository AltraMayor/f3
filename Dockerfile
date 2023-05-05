FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc \
        less \
        libparted-dev \
        libudev1 \
        libudev-dev \
        make \
        udev

COPY . /f3

WORKDIR /f3

RUN make install

RUN make install-extra

CMD less README.rst
