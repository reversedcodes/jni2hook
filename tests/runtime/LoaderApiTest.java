import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;

/* Drives JNI2Hook_FindLoadedClass, JNI2Hook_GetClassLoader,
   JNI2Hook_DefineClass and JNI2Hook_RemapClass against a live VM.

   RemapPlugin is compiled against RemapApi and RemapValue, neither of which is
   on this class path. Only the obfuscated ra and rv are. So the plugin can
   only load, link and run if the remapper rewrote its super class, its
   inherited method reference and its descriptors, which is what the last
   assertion actually proves. */
public class LoaderApiTest {

    static native boolean findLoadedClass(String internalName);

    static native boolean findMissingClass(String internalName);

    static native String loaderNameOf(String internalName);

    static native Class<?> defineRemapped(String internalName, byte[] classBytes);

    static native void shutdown();

    private static int failures = 0;

    private static void check(String what, boolean condition) {
        System.out.printf("  %-58s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) {
            failures++;
        }
    }

    private static void equals(String what, Object actual, Object expected) {
        boolean ok = actual == null ? expected == null : actual.equals(expected);
        System.out.printf("  %-44s = %-24s %s%n", what, actual, ok ? "ok" : "FAIL <" + expected + ">");
        if (!ok) {
            failures++;
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path pluginClass = Path.of(args[1]);

        // Force the obfuscated classes to load, so GetLoadedClasses can see them.
        Class<?> runtimeBase = Class.forName("ra");
        Class.forName("rv");

        System.out.println("== find a loaded class ==");
        check("finds ra", findLoadedClass("ra"));
        check("finds rv", findLoadedClass("rv"));
        check("finds a JDK class", findLoadedClass("java/lang/String"));
        check("reports NOT_FOUND for a class nobody loaded",
                findMissingClass("no/such/Class"));

        System.out.println("== resolve the defining loader ==");
        equals("loader of ra", loaderNameOf("ra"), runtimeBase.getClassLoader().getName());
        equals("loader of java/lang/String", loaderNameOf("java/lang/String"), "<bootstrap>");

        System.out.println("== remap and define from memory ==");
        byte[] bytes = Files.readAllBytes(pluginClass);
        Class<?> plugin = defineRemapped("RemapPlugin", bytes);
        check("DefineClass returned a class", plugin != null);

        if (plugin != null) {
            check("landed in ra's loader", plugin.getClassLoader() == runtimeBase.getClassLoader());
            equals("super class after remapping", plugin.getSuperclass().getName(), "ra");

            Method run = plugin.getDeclaredMethod("run", Class.forName("rv"));
            equals("descriptor after remapping", run.getParameterTypes()[0].getName(), "rv");

            Object value = Class.forName("rv").getDeclaredConstructor().newInstance();
            Object result = run.invoke(plugin.getDeclaredConstructor().newInstance(), value);
            equals("calling it runs the remapped methodref", result, "plugin:api:runtime:runtime");
        }

        shutdown();
        System.out.println(failures == 0 ? "PASS" : failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
