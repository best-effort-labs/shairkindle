package com.amazon.kindle.kindlet;

/**
 * Clean-room COMPILE-TIME stub of Amazon's Kindlet lifecycle interface.
 *
 * This is our own minimal declaration of the public API surface our kindlet
 * uses -- NOT Amazon's code. The real implementation is supplied by the Kindle
 * framework at runtime; these stub classes are NOT bundled into the .azw2 (see
 * build-sign.sh). Signatures verified against the on-device Kindlet-1.2.jar.
 */
public interface Kindlet {
    void create(KindletContext context);
    void start();
    void stop();
    void destroy();
}
