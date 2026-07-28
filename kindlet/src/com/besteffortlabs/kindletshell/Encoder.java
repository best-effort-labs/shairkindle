package com.besteffortlabs.kindletshell;

public interface Encoder { byte[] encode(OutboundEvent e, int seq); }
