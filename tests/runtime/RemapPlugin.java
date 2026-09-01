/* Compiled against the readable names and never against ra or rv. Loading it
   unchanged would fail with NoClassDefFoundError, so every assertion the test
   makes about it is an assertion about the remapper.

   It covers three kinds of reference at once: the super class, an inherited
   method call, and a descriptor that names a remapped type. */
public class RemapPlugin extends RemapApi {

    public String run(RemapValue value) {
        return "plugin:" + greet(value) + ":" + value.text();
    }
}
