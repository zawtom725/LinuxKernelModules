### Development decisions
1. In get_inode_side(inode): I use a fixed-size buffer of 32 bytes to get the extended attribute of the inode, because all valid attributes are less than 31-byte long. There is no need to differentiate between buffer-too-small and no-such-attribute errors of getxattr().
2. In mp4_inode_init_security(...): If the new inode is a file, I put "read-write" attribute on it. If it is a new directory, I put "dir-write" attribute on it.
3. In mp4_has_permission(...): The operation mask also includes bit for open/chdir operations. In practice: 
    * open is set together with read/write/append bit
    * chdir is set together with write bit for directories.
    However, I am not going to make any uninformed assumptions about these operations since the docs does not say anything about them. Therefore, If I get an operation mask with ONLY open/chdir bits set, I permit the operation.
4. In mp4_inode_permission(...): Whenever I reject an operation mask, I take log, as required in the docs: 
  `mp4_inode_permission denied [/sample/path]`
  alongside with additional information about why the operation is rejected. (can be set by the DEBUG flag in mp4.c):
  `mp4 not permitted: ncoaprwe [1][0][0][0][0][1][0][0] ssid [7] osid [0] mask [132] is_dir [0]`

### Testing
My module is fully functional. It exhibits exactly the same behaviors as specified in the docs.
##### Test 1
We firstly test for access control.
Assume a text file at /home/ziangw2/a.txt, we have the following test.perm:
```shell
sudo setfattr -n security.mp4 -v dir /etc
sudo setfattr -n security.mp4 -v dir /etc/ld.so.cache
sudo setfattr -n security.mp4 -v dir /home/ziangw2
sudo setfattr -n security.mp4 -v read-only /home/ziangw2/out.txt
```
And test.perm.unload, which basically undos test.perm:
```shell
sudo setfattr -x security.mp4 /etc
sudo setfattr -x security.mp4 /etc/ld.so.cache
sudo setfattr -x security.mp4 /home/ziangw2
sudo setfattr -x security.mp4 read-only /home/ziangw2/out.txt
```
Then, we run the following shell commands one by one:
```shell
cat a.txt
# a.txt printed out
sudo setfattr -n security.mp4 -v target /bin/cat
cat a.txt
# permission denied
source test.perm
cat a.txt
# a.txt printed out
source test.perm.unload
cat a.txt
# permission denied
sudo setfattr -x security.mp4 /bin/cat
cat a.txt
# a.txt printed out
```
There are five `cat`. The first, third and fifth `cat` are permitted while the second and the fourth are rejected. The behavior is correct.
##### Test 2
Then, we test for both access control and inode_init_security() function.
We have test.perm:
```shell
sudo setfattr -n security.mp4 -v dir /etc
sudo setfattr -n security.mp4 -v read-only /etc/ld.so.cache
sudo setfattr -n security.mp4 -v read-only /etc/locale.alias
sudo setfattr -n security.mp4 -v dir-write /home/ziangw2
```
And test.perm.unload, which basically undos the above:
```shell
sudo setfattr -x security.mp4 /etc
sudo setfattr -x security.mp4 /etc/ld.so.cache
sudo setfattr -x security.mp4 /etc/locale.alias
sudo setfattr -x security.mp4 /home/ziangw2
```
Then, we run the following shell commands one by one:
```shell
touch a.txt
# a.txt created
getfattr -n security.mp4 a.txt
# security.mp4: No such attribute
sudo setfattr -n security.mp4 -v target /bin/touch
touch b.txt
# permission denied
source test.perm
touch c.txt
# c.txt created
getfattr -n security.mp4 c.txt
# security.mp4="read-write"
source test.perm.unload
touch d.txt
# permission denied
sudo setfattr -x security.mp4 /bin/touch
touch e.txt
# e.txt created
getfattr -n security.mp4 e.txt
# security.mp4: No such attribute
```
There are five `touch`: a.txt, b.txt, c.txt, d.txt, e.txt. Among the five, A, C and E succeeds. Among A, C and E, only C gets labeled "read-write". B and D are rejected. The behavior is correct.

### Least Privilege Policy for /usr/bin/passwd
##### passwd.perm content
Firstly, I run the following shell commands to add the dummy user "mp4" and obtain a strace report for files accessed by /usr/bin/passwd
```shell
sudo useradd mp4
sudo apt-get install strace
sudo strace -o report.txt -f -t -c trace=file passwd mp4
```
Then, I write a python script (really!) to analyze the report. I extract all the logs for `open`. I translate the flags used by open directly to the security labels for all the files opened by /usr/bin/passwd. For example`32535 16:33:05 open("/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3` would translate to `sudo setfattr -n security.mp4 -v read-only /etc/ld.so.cache`. I put all the translated log commands into passwd.perm. The passwd.perm now looks like:
```shell
sudo setfattr -n security.mp4 -v write-only /etc/.pwd.lock
sudo setfattr -n security.mp4 -v read-write /etc/krb5.conf
sudo setfattr -n security.mp4 -v read-only /etc/ld.so.cache
sudo setfattr -n security.mp4 -v read-only /etc/localtime
# more stuff
```
Then. I set the security label for /usr/bin/passwd to target, as required in the docs. I add the following line to passwd.perm:
```shell
sudo setfattr -n security.mp4 -v target /usr/bin/passwd
```
I then run `sudo passwd mp4`. I use my dmesg log of all the failed access attempts of passwd to see what other files and directories are accessed. I determine the security label needed for each extra files and directories using the additional informations (see my 4th design decision). I add these to passwd.perm:
```shell
sudo setfattr -n security.mp4 -v dir-write /etc
sudo setfattr -n security.mp4 -v dir /etc/pam.d
sudo setfattr -n security.mp4 -v dir /etc/security
sudo setfattr -n security.mp4 -v read-only /var/lib/sss/mc/passwd
sudo setfattr -n security.mp4 -v read-only /var/run/utmp
sudo setfattr -n security.mp4 -v read-only /dev/urandom
```
Afterwards, I run `sudo passwd mp4` again. This time, I successfully change the password for dummy user mp4.
##### passwd.perm.unload content
It basically just undos whatever is done in passwd.perm by using `sudo setfattr -x security.mp4 /path/file`.

##### Gaurantee of least privilege
1. All the files opened by /usr/bin/passwd are assigned proper security labels.
2. I add security labels for 6 extra files and directores in a way such that all six are required for /usr/bin/passwd to succeed.
3. Except for setting /usr/bin/passwd as target, no other security labels are assigned.

##### Things to notice:
1. passwd also requires writing permission to /run/systemd/journal/dev-log. I can not assign a security label to that file because, under our MAC, the label will prevent other normal programs from writing to it. Whether passwd has writing permission to dev-log does not affect the outcome of running `sudo passwd mp4`.
2. passwd also requires reading permission to /proc/filesystems, /proc/fs, /proc/5500 (some number, changes everytime). I can not assign security labels to them since setfattr is not supported for the proc fs. According to [my piazza post](https://piazza.com/class/jcgqvneo9tn1o0?cid=491), we don't need to worry about that. Whether passwd has reading permission to the proc fs does not affect the outcome of running `sudo passwd mp4`.

##### Test that my module correctly implements passwd.perm
Given everything above, run the below shell commands one by one:
```shell
sudo passwd mp4
# passwd succeeds
sudo setfattr -n security.mp4 -v target /usr/bin/passwd
sudo passwd mp4
# permission denied: can not determinue your username
source passwd.perm
sudo passwd mp4
# passwd succeeds
sudo setfattr -x security.mp4 /usr/bin/passwd
sudo passwd mp4
# permission denied: authentication manipulation error (no write permission)
source passwd.perm.unload
# /usr/bin/passwd: no such attribute
sudo passwd mp4
# passwd succeeds
```
The logic is the same as the previous two tests.
