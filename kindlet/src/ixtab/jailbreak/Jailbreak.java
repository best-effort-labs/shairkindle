/* Imported ixtab kindlejailbreak frontend, WTFPL. Provenance: ../README.md. */

package ixtab.jailbreak;

import java.security.Permission;
import java.security.PermissionCollection;
import java.security.Permissions;
import java.security.Policy;
import java.security.ProtectionDomain;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Map;

import org.json.simple.parser.ParseException;

public class Jailbreak implements JailbreakConstants {

	protected final ParseException gateway;
	private final boolean available;
	protected final Object lock = new Object();
	protected final Class domainClass;

	private Accessor accessor;
	private Context context;

	public Jailbreak() {
		this(Jailbreak.class);
	}
	
	protected Jailbreak(Class domainClass) {
		this(domainClass, new ParseException(JAILBREAK, MY, KINDLE));
	}

	protected Jailbreak(Class domainClass, ParseException gateway) {
		this.domainClass = domainClass;
		this.gateway = gateway;
		available = gateway.toString().equals(JAILBREAKER);
	}

	protected static boolean isJailbreakerObject(Object o) {
		return o != null
				&& (o.getClass().getName().toLowerCase().indexOf(JAILBREAKER) != -1);
	}

	public boolean isAvailable() {
		return available;
	}

	private void checkAvailable() {
		if (!isAvailable()) {
			throw new IllegalStateException(
					"Jailbreak library is not installed on this Kindle, sorry.");
		}
	}

	public boolean isEnabled() {
		if (!isAvailable())
			return false;
		return getContext().isJailBroken;
	}

	protected final void setContext(Context c) {
		synchronized (lock) {
			context = c;
		}
	}

	public final Context getContext() {
		synchronized (lock) {
			if (context == null && isAvailable()) {
				context = new Context(null);
			}
			return context;
		}
	}

	public boolean enable() {
		if (!isAvailable()) {
			return false;
		}
		Context c = new Context(Boolean.TRUE);
		setContext(c);
		return c.isJailBroken;
	}

	public boolean disable() {
		if (!isAvailable()) {
			return false;
		}
		Context c = new Context(Boolean.FALSE);
		setContext(c);
		return !c.isJailBroken;
	}

	protected final Accessor getAccessor() {
		if (accessor == null) {
			synchronized (lock) {
				if (accessor == null) {
					accessor = instantiateAccessor();
				}
			}
		}
		return accessor;
	}

	protected Accessor instantiateAccessor() {
		return new Accessor();
	}

	public Metadata getMetadata() {
		return new Metadata(true, true);
	}
	
	public static class Metadata {
		public boolean autoEnable;
		public boolean autoDisable;
		
		
		/**
		 * Blabla.
		 * 
		 * @param autoEnable foo
		 * @param autoDisable bar
		 */
		public Metadata(boolean autoEnable, boolean autoDisable) {
			super();
			this.autoEnable = autoEnable;
			this.autoDisable = autoDisable;
		}
	}
	
	public class Accessor {

		protected Accessor() {
			checkAvailable();
		}

		protected Map createInput(String cmd) {
			Map input = new HashMap();
			input.put(Protocol.COMMAND, cmd);
			return input;
		}

		protected Map invoke(Map input) {
			synchronized (gateway) {
				gateway.setUnexpectedObject(input);
				Map output = (Map) gateway.getUnexpectedObject();
				return output;
			}
		}

		public ProtectionDomain getProtectionDomain(Class clazz) {
			Map input = createInput(Protocol.PROTECTION_DOMAIN);
			input.put(Protocol.PROTECTION_DOMAIN_CLASS, clazz);
			Map output = invoke(input);
			return (ProtectionDomain) output.get(Protocol.PROTECTION_DOMAIN);
		}

		public Policy getPolicy(Boolean replaceWith) {
			Map input = createInput(Protocol.POLICY);
			input.put(Protocol.POLICY_REPLACE_WITH, replaceWith);
			Map output = invoke(input);
			return (Policy) output.get(Protocol.POLICY);
		}

		public int getVersion() {
			Map input = createInput(Protocol.VERSION);
			Map output = invoke(input);
			return ((Integer) output.get(Protocol.VERSION)).intValue();
		}

		public ClassLoader getClassLoader(ClassLoader current) {
			Map input = createInput(Protocol.CLASSLOADER);
			input.put(Protocol.CLASSLOADER_PARENT, current);
			Map output = invoke(input);
			return (ClassLoader) output.get(Protocol.CLASSLOADER);
		}
	}

	public class Context {
		public final int versionFrontEnd = VERSION;
		public final int versionBackEnd;
		public final boolean isJailBroken;

		public final ProtectionDomain protectionDomain;
		public final Policy policy;
		public final PermissionCollection permissions;
		public final ClassLoader classLoader;

		public String toString() {
			StringBuffer sb = new StringBuffer(super.toString());
			sb.append(":\n");

			sb.append("frontend version: ");
			sb.append(versionFrontEnd);
			sb.append("\n");

			sb.append("backend version: ");
			sb.append(versionBackEnd);
			sb.append("\n");

			sb.append("jailbroken state: ");
			sb.append(isJailBroken);
			sb.append("\n");

			sb.append("policy: ");
			sb.append(policy == null ? "null" : policy.getClass().getName());
			sb.append("\n");

			sb.append("permissions: ");
			sb.append(permissions == null ? "null" : permissions.getClass()
					.getName());
			sb.append("\n");

			sb.append("additional permissions enabled: ");
			sb.append(hasAdditionalPermissions());
			sb.append("\n");
			
			sb.append("classloader: " + classLoader+ "\n");
			return sb.toString();
		}

		public Context(Boolean replacePolicy) {
			Accessor accessor = getAccessor();
			versionBackEnd = accessor.getVersion();
			classLoader = accessor.getClassLoader(domainClass.getClassLoader());
			protectionDomain = accessor.getProtectionDomain(domainClass);
			policy = accessor.getPolicy(replacePolicy);
			if (policy != null) {
				permissions = policy.getPermissions(protectionDomain);
			} else {
				permissions = null;
			}
			isJailBroken = isJailbreakerObject(permissions);
		}

		public boolean requestPermission(Permission permission) {
			Permissions permissions = new Permissions();
			permissions.add(permission);
			return requestPermissions(permissions);
		}

		public boolean requestPermissions(Permissions permissions) {
			if (!isJailBroken)
				return false;
			boolean allOk = true;
			Enumeration list = permissions.elements();
			while (list.hasMoreElements()) {
				Permission request = (Permission) list.nextElement();
				if (!policy.implies(protectionDomain, request)) {
					this.permissions.add(request);
					allOk &= policy.implies(protectionDomain, request);
				}
			}
			return allOk;
		}

		public boolean hasAdditionalPermissions() {
			if (!isJailBroken)
				return false;
			return !permissions.isReadOnly();
		}

		public boolean clearAdditionalPermissions() {
			if (!isJailBroken)
				return false;
			permissions.setReadOnly();
			return permissions.isReadOnly();
		}
	}

}
