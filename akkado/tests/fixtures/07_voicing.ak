// User-defined voicing dict. Exercises the F14 voicing_registry path
// (codegen_patterns.cpp:4904 register + :4538 lookup).
addVoicings("snapshot_jazz", {M: [0, 4, 7, 11], m: [0, 3, 7, 10]})
c"CM Am" .voicing("snapshot_jazz")
    |> poly(@, ({freq}) -> sine(freq))
    |> out(@, @)
