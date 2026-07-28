package com.amazon.kindle.kindlet;

import java.awt.Container;
import java.io.File;

/**
 * Clean-room COMPILE-TIME stub of Amazon's KindletContext -- ONLY the members
 * our kindlet actually calls (the real interface has more). Our code, not
 * Amazon's; not bundled into the .azw2. Signatures verified against the
 * on-device Kindlet-1.2.jar via `javap`.
 */
public interface KindletContext {
    Container getRootContainer();
    File getHomeDirectory();
    void setSubTitle(String subtitle);
}
