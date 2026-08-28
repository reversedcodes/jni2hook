import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.util.ArrayList;
import java.util.List;

/* Reaches all four type_annotation targets that carry a bytecode offset inside
   a Code attribute, which is the one place the opaque-attribute rule does not
   hold: NEW and INSTANCEOF are offset_target, the cast is type_argument_target,
   and the annotated locals are localvar_target, a span. Nothing else in the
   corpus produces them, and an insertion that leaves them alone makes them name
   the inserted instruction instead of the one they were written on. */
public class TypeAnnotated {

    @Target({ElementType.TYPE_USE})
    @Retention(RetentionPolicy.RUNTIME)
    @interface NonNull {}

    public int run(Object o, int n) {
        @NonNull List<String> xs = new @NonNull ArrayList<>();
        for (int i = 0; i < n; i++) {
            if (o instanceof @NonNull String) {
                xs.add((@NonNull String) o);
            } else {
                @NonNull Object other = new @NonNull Object();
                xs.add(other.toString());
            }
        }
        return xs.size();
    }

    public @NonNull String describe(@NonNull Object o) {
        try {
            return ((@NonNull String) o).trim();
        } catch (ClassCastException e) {
            return String.valueOf(o);
        }
    }
}
