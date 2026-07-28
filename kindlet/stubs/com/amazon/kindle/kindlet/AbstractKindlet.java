package com.amazon.kindle.kindlet;

/**
 * Clean-room COMPILE-TIME stub of Amazon's AbstractKindlet (no-op lifecycle
 * base). Our code, not Amazon's; not bundled into the .azw2 -- the framework
 * provides the real class at runtime. See Kindlet.java.
 */
public abstract class AbstractKindlet implements Kindlet {
    public void create(KindletContext context) { }
    public void start() { }
    public void stop() { }
    public void destroy() { }
}
