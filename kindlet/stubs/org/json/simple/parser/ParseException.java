/*
 * COMPILE-ONLY clean-room stub. NOT shipped in the .azw2.
 *
 * The real org.json.simple.parser.ParseException lives on the device inside
 * /opt/amazon/ebook/lib/json_simple-1.1.jar, patched by ixtab's backend so that
 * the magic-triple constructor + setUnexpectedObject()/getUnexpectedObject()
 * form the runtime gateway to ixtab.jailbreak.backend. We must NOT bundle this
 * class (`build-sign.sh` hard-fails if it lands in the archive) so the device's
 * patched copy wins at load time. This stub only satisfies the compiler for the
 * three members ixtab's Jailbreak frontend references. Signatures match the real
 * class's public surface (verified against the ixtab source).
 */
package org.json.simple.parser;

public class ParseException extends Exception {

    public ParseException(int position, int errorType, Object unexpectedObj) {
    }

    public void setUnexpectedObject(Object unexpectedObject) {
    }

    public Object getUnexpectedObject() {
        return null;
    }
}
