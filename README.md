# wazo-libsccp

An Asterisk channel driver for the SCCP protocol.

This work is based on the Asterisk chan\_skinny originally written by Jeremy
McNamara & Florian Overkamp.

## Getting Started

* http://documentation.wazo.community/en/stable/contributors/sccp.html
* https://github.com/wazo-platform/wazo-libsccp

## buildh
[buildh](./buildh) is a python script used to compile and install the sccp channel driver on a remote wazo stack.
You need a configuration file with the following content, by default located at `~/.wazo-libsccp-buildh-config`:
```
[default]
host = pcm-dev-primary
directory = wazo-libsccp
```
Replace pcm-dev-primary by the hostname of your stack.

Then on your Wazo stack install `asterisk-dev`:
`apt update && apt install asterisk-dev`

Finally you should be able to launch `./buildh make` to compile the sccp channel driver on your stack and `./buildh makei` to build and install it.
