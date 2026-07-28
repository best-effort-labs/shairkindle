/* Imported ixtab kindlejailbreak frontend, WTFPL. Provenance: ../README.md. */

package ixtab.jailbreak;

import ixtab.jailbreak.Jailbreak;

import com.amazon.kindle.kindlet.AbstractKindlet;
import com.amazon.kindle.kindlet.KindletContext;

public abstract class SuicidalKindlet extends AbstractKindlet {

	protected final Jailbreak jailbreak = instantiateJailbreak();
	private final AbstractKindlet delegate;

	/**
	 * Constructs a <code>SuicidalKindlet</code> which wants to use an
	 * unrestricted class loader. This constructor is equivalent to
	 * <code>SuicidalKindlet(true)</code>. If your application does not require
	 * unrestricted class access, use
	 * {@link SuicidalKindlet#SuicidalKindlet(boolean)} with an argument of
	 * <code>false</code> instead.
	 */
	public SuicidalKindlet() {
		this(true);
	}

	/**
	 * Constructs a <code>SuicidalKindlet</code> which tries (or does not try)
	 * to escape the restricted Kindle class loader on instantiation based on
	 * the <code>replaceClassLoader</code> parameter.
	 * 
	 * @param replaceClassLoader
	 *            if <code>true</code>, try to escape the restricted class
	 *            loader, replacing it with a fully functional one. Whether the
	 *            request has actually succeeded can be determined by overriding
	 *            {@link SuicidalKindlet#instantiateDelegate(AbstractKindlet)}.
	 */
	public SuicidalKindlet(boolean replaceClassLoader) {
		AbstractKindlet del = null;
		if (replaceClassLoader && jailbreak.isAvailable()) {
			ClassLoader cl = jailbreak.getContext().classLoader;
			if (cl != this.getClass().getClassLoader()) {
				try {
					Class better = cl.loadClass(this.getClass().getName());
					del = (AbstractKindlet) better.newInstance();
				} catch (Throwable t) {
					throw new RuntimeException(t);
				}
			}
		}
		this.delegate = replaceClassLoader ? instantiateDelegate(del) : del;
	}

	/**
	 * This method is invoked as a callback if class loader replacement is
	 * requested. It gives subclasses a chance to verify the proposed candidate,
	 * and to veto it if required.
	 * 
	 * @param candidate
	 *            a proposed instance of the subclass, but loaded in an
	 *            unrestricted class loader. This may be <code>null</code> if
	 *            the jailbreak is not installed or another error occurred. In
	 *            this case, the jailbreak will usually not be able to escape the
	 *            restricted class loader.
	 * @return the unrestricted instance that should be used for the
	 *         application, or <code>null</code> if the restricted classloader
	 *         is to be used. The default implementation returns the
	 *         <code>candidate</code> that was passed in.
	 */
	protected AbstractKindlet instantiateDelegate(AbstractKindlet candidate) {
		return candidate;
	}

	protected Jailbreak instantiateJailbreak() {
		return new Jailbreak();
	}

	public final void create(KindletContext context) {
		if (jailbreak.isAvailable() && jailbreak.getMetadata().autoEnable) {
			jailbreak.enable();
		}
		if (delegate != null) {
			delegate.create(context);
			return;
		}
		onCreate(context);
	}

	public final void start() {
		if (delegate != null) {
			delegate.start();
			return;
		}
		onStart();
	}

	public final void stop() {
		if (delegate != null) {
			delegate.stop();
			return;
		}
		onStop();
	}

	public final void destroy() {
		onDestroy();
		if (jailbreak.isAvailable() && jailbreak.getMetadata().autoDisable) {
			jailbreak.disable();
		}
		if (delegate != null) {
			delegate.destroy();
		}
	}

	protected void onCreate(KindletContext context) {
		super.create(context);
	}

	protected void onStart() {
		super.start();
	}

	protected void onStop() {
		super.stop();
	}

	protected void onDestroy() {
		super.destroy();
	}

}
