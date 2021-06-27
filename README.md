# Demos pertaining to files and filesystems

For teaching and training.

Minimum required versions are *Linux 4.5* and *libc 2.27*.  
Not supported are versions of a requirement older than two years
(including later patch releases), except its latest minor release;
counted from January 1st of the current year.

## joinfiles

This is a replacement for
```sh
cat a b c… >d
```

… that demonstrates efficient collation of a regular file `d` from its parts `a, b…`:
```sh
joinfiles a b c d
```

### Example
```bash
if command -v podman &>/dev/null; then
  : ${reference:="$(command -v podman)"}
else
  : ${reference:="/usr/bin/docker"}
fi

for n in {0..2}; do
  dd if="${reference}" skip=$(( 40960 * $i )) count=40960 of="f${i}"
done

./joinfiles f{0..2} whole
cmp "${reference}" whole
```
