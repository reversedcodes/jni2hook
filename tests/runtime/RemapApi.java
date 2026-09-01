/* The readable class a plugin is compiled against. It never exists at runtime:
   the VM only has the obfuscated ra, which is what the remapper has to make
   the plugin link against. */
public class RemapApi {

    public String greet(RemapValue value) {
        return "api:" + value.text();
    }
}
