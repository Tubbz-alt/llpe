; RUN: llc < %s -march=x86 | grep movw | count 1

define signext i16 @t()   {
entry:
        %tmp180 = load i16* null, align 2               ; <i16> [#uses=3]
        %tmp180181 = sext i16 %tmp180 to i32            ; <i32> [#uses=1]
        %tmp182 = add i16 %tmp180, 10
        %tmp185 = icmp slt i16 %tmp182, 0               ; <i1> [#uses=1]
        br i1 %tmp185, label %cond_true188, label %cond_next245

cond_true188:           ; preds = %entry
        %tmp195196 = trunc i16 %tmp180 to i8            ; <i8> [#uses=0]
        ret i16 %tmp180

cond_next245:           ; preds = %entry
        %tmp256 = and i32 %tmp180181, 15                ; <i32> [#uses=0]
        %tmp3 = trunc i32 %tmp256 to i16
        ret i16 %tmp3
}
