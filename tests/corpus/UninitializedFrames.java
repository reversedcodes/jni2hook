import java.util.ArrayList;
import java.util.List;

/* Puts a StackMapTable frame in the middle of an object construction, which is
   the only way an Uninitialized verification type ends up naming an instruction
   that an insertion can move.
 *
 * A branch inside the argument list of a new expression forces javac to emit a
 * frame while the half-built reference is still on the stack, so that frame
 * carries `uninitialized <offset of the new>`. Inserting at exactly that offset
 * has to move the entry along with the new it names, unlike a branch target,
 * which stays put so the inserted code takes it over. Nothing else in the
 * corpus reaches that case. */
public class UninitializedFrames {

    public String pick(boolean flag, int n) {
        StringBuilder sb = new StringBuilder(flag ? "yes" : "no");
        for (int i = 0; i < n; i++) {
            sb.append(i % 2 == 0 ? '.' : '-');
        }
        return sb.toString();
    }

    public List<String> build(boolean flag, String a, String b) {
        List<String> out = new ArrayList<>(flag ? 4 : 16);
        out.add(new String(flag ? a : b));
        out.add(new StringBuilder(a == null ? "" : a).reverse().toString());
        return out;
    }

    public Object nested(boolean flag, int n) {
        return new StringBuilder(new StringBuilder(flag ? "a" : "b").append(n).toString());
    }
}
