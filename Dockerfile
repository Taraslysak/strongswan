FROM androidsdk/android-30:latest

WORKDIR /strongswan

RUN apt-get update
RUN apt-get install gcc automake autoconf libtool pkg-config gettext perl python flex bison gperf cmake libgmp3-dev -y