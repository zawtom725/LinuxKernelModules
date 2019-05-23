// hooks and security module
#include <linux/lsm_hooks.h>
#include <linux/security.h>
// kernels
#include <linux/kernel.h>
#include <linux/err.h>
// credential struct
#include <linux/cred.h>
// kmalloc
#include <linux/slab.h>
// struct file, linux_binprm
#include <linux/dcache.h>
#include <linux/binfmts.h>
// getattr
#include <linux/fs.h>
// strings
#include <linux/string.h>
// S_ISDIR
#include <linux/stat.h>
// printk_ratelimited
#include <linux/printk.h>
// struct, flag vals and helper functions
#include "mp4_given.h"


// kernel booting printing
#define pr_fmt(fmt) "cs423_mp4: " fmt
// compile flag
#define DEBUG 	1
// size of buffers
#define MP4_XATTR_BUF_SIZE	32
#define MP4_PATH_BUF_SIZE	256
// constants
#define MP4_RDWR_STR		"read-write"
#define MP4_DIRWR_STR		"dir-write"
// access macro
#define SUBJECTIVE_BLOB(cred)	((struct mp4_security*) cred->security)


/**
 * get_inode_sid - Get the inode mp4 security label id
 *
 * @inode: the input inode
 *
 * @return the inode's security id if found.
 *
 */
static int get_inode_sid(struct inode *inode){
	struct dentry *dent;
	char *buf;
	int flag;
	ssize_t ret_code;

	// get dentry
	dent = d_find_alias(inode);
	// test for getxattr, minimal access privilege
	if(dent == NULL){
		return MP4_NO_ACCESS;
	}else if(inode->i_op->getxattr == NULL){
		dput(dent);
		return MP4_NO_ACCESS;
	}

	// get extended attribute in string
	buf = (char*) kmalloc(MP4_XATTR_BUF_SIZE, GFP_KERNEL);
	strncpy(buf, "", MP4_XATTR_BUF_SIZE);

	ret_code = inode->i_op->getxattr(dent, XATTR_NAME_MP4, buf, MP4_XATTR_BUF_SIZE);
	dput(dent);

	// don't care whether it is buffer overflow or failure, just return minial access privilege
	if(ret_code < 0){
		kfree(buf);
		return MP4_NO_ACCESS;
	}

	// inline convert string to code
	flag = __cred_ctx_to_sid(buf);
	kfree(buf);

	return flag;
}

/**
 * mp4_bprm_set_creds - Set the credentials for a new task
 *
 * @bprm: The linux binary preparation structure
 *
 * returns 0 on success.
 */
static int mp4_bprm_set_creds(struct linux_binprm *bprm){
	struct cred *this_cred;
	int inode_sid;

	this_cred = bprm->cred;
	inode_sid = get_inode_sid(bprm->file->f_inode);

	// get inode code
	if(inode_sid == MP4_TARGET_SID){
		// allocate new security blob if needed, might not be called at all
		if(this_cred->security == NULL){
			this_cred->security = (struct mp4_security*) kmalloc(sizeof(struct mp4_security), GFP_KERNEL);
		}
		SUBJECTIVE_BLOB(this_cred)->mp4_flags = MP4_TARGET_SID;

		#ifdef DEBUG
		pr_info("mp4_bprm_set_creds get [%d] as inode_sid and set to [%d]\n", inode_sid, SUBJECTIVE_BLOB(bprm->cred)->mp4_flags);
		#endif
	}

	return 0;
}

/**
 * mp4_cred_alloc_blank - Allocate a blank mp4 security label
 *
 * @cred: the new credentials
 * @gfp: the atomicity of the memory allocation
 *
 */
static int mp4_cred_alloc_blank(struct cred *cred, gfp_t gfp){
	cred->security = (struct mp4_security*) kmalloc(sizeof(struct mp4_security), gfp);
	SUBJECTIVE_BLOB(cred)->mp4_flags = MP4_NO_ACCESS;

	return 0;
}


/**
 * mp4_cred_free - Free a created security label
 *
 * @cred: the credentials struct
 *
 */
static void mp4_cred_free(struct cred *cred){
	kfree(cred->security);
}

/**
 * mp4_cred_prepare - Prepare new credentials for modification
 *
 * @new: the new credentials
 * @old: the old credentials
 * @gfp: the atomicity of the memory allocation
 *
 */
static int mp4_cred_prepare(struct cred *new, const struct cred *old, gfp_t gfp){
	int old_flag;

	// get old flag
	if(old->security == NULL){
		old_flag = MP4_NO_ACCESS;
	}else{
		old_flag = SUBJECTIVE_BLOB(old)->mp4_flags;
	}

	// copy to new
	if(new->security == NULL){
		new->security = (struct mp4_security*) kmalloc(sizeof(struct mp4_security), gfp);
	}
	SUBJECTIVE_BLOB(new)->mp4_flags = old_flag;

	return 0;
}

/**
 * mp4_inode_init_security - Set the security attribute of a newly created inode
 *
 * @inode: the newly created inode
 * @dir: the containing directory
 * @qstr: unused
 * @name: where to put the attribute name
 * @value: where to put the attribute value
 * @len: where to put the length of the attribute
 *
 * returns 0 if all goes well, -ENOMEM if no memory, -EOPNOTSUPP to skip
 *
 */
static int mp4_inode_init_security(struct inode *inode,
									struct inode *dir,
				   					const struct qstr *qstr,
				   					const char **name,
				   					void **value,
				   					size_t *len){
	const struct cred *this_cred;

	char *name_buf;
	char *val_buf;

	// get the subjective blob of current running task
	this_cred = current_cred();

	// test whether is target
	if(this_cred == NULL || 
		this_cred->security == NULL || 
		SUBJECTIVE_BLOB(this_cred)->mp4_flags != MP4_TARGET_SID){
		return -EOPNOTSUPP;
	}

	#ifdef DEBUG
	pr_info("mp4_inode_init_security target pgrm\n");
	#endif

	// set xattr, create buffer
	val_buf = (char*) kmalloc(MP4_XATTR_BUF_SIZE, GFP_KERNEL);
	name_buf = (char*) kmalloc(MP4_XATTR_BUF_SIZE, GFP_KERNEL);
	if(val_buf == NULL || name_buf == NULL){
		return -ENOMEM;
	}

	strncpy(name_buf, XATTR_MP4_SUFFIX, MP4_XATTR_BUF_SIZE);
	// set value based on whether it is directory or file
	if(S_ISDIR(inode->i_mode)){
		strncpy(val_buf, MP4_DIRWR_STR, MP4_XATTR_BUF_SIZE);
	}else{
		strncpy(val_buf, MP4_RDWR_STR, MP4_XATTR_BUF_SIZE);
	}
	
	// set return values
	*name = name_buf;
	*value = val_buf;
	*len = strlen(val_buf);

	return 0;
}

/**
 * mp4_has_permission - Check if subject has permission to an object
 *
 * @ssid: the subject's security id
 * @osid: the object's security id
 * @mask: the operation mask
 *
 * returns 0 is access granter, -EACCESS otherwise
 *
 */
static int mp4_has_permission(int ssid, int osid, int mask, bool is_dir){
	bool read;
	bool write;
	bool exec;
	bool access;
	bool append;
	bool open;
	bool chdir;
	bool not_block;

	bool permit;

	// bit masking
	exec = (bool) (mask & MAY_EXEC);
	write = (bool) (mask & MAY_WRITE);
	read = (bool) (mask & MAY_READ);
	access = (bool) (mask & MAY_ACCESS);
	append = (bool) (mask & MAY_APPEND);
	// unspecified ops, no enforcement
	open = (bool) (mask & MAY_OPEN);
	chdir = (bool) (mask & MAY_CHDIR);
	not_block = (bool) (mask & MAY_NOT_BLOCK);

	// minimal access privilege
	permit = false;

	// target program policy enforcement
	if(ssid == MP4_TARGET_SID){
		if(is_dir){
			switch(osid){
				case MP4_READ_DIR:
					if((access || read || exec) && !write && !append){
						permit = true;
					}
					break;
				case MP4_RW_DIR:
					if((access || read || exec || write || append)){
						permit = true;
					}
					break;
			}
		}else{
			switch(osid){
				case MP4_NO_ACCESS:
					break;
				case MP4_READ_OBJ:
					if((access || read) && !exec && !write && !append){
						permit = true;
					}
					break;
				case MP4_READ_WRITE:
					if((access || read || write || append) && !exec){
						permit = true;
					}
					break;
				case MP4_WRITE_OBJ:
					if((access || write || append) && !read && !exec){
						permit = true;
					}
					break;
				case MP4_EXEC_OBJ:
					if((access || read || exec) && !write && !append){
						permit = true;
					}
					break;
			}
		}	
	}
	// non-target program policy enforement
	else{
		if(is_dir){
			permit = true;
		}else{
			switch(osid){
				case MP4_NO_ACCESS:
					permit = true;
					break;
				case MP4_READ_OBJ:
					if((access || read) && !exec && !write && !append){
						permit = true;
					}
					break;
				case MP4_READ_WRITE:
					if((access || read) && !exec && !write && !append){
						permit = true;
					}
					break;
				case MP4_WRITE_OBJ:
					if((access || read) && !exec && !write && !append){
						permit = true;
					}
					break;
				case MP4_EXEC_OBJ:
					if((access || read || exec) && !write && !append){
						permit = true;
					}
					break;
			}
		}
	}

	// only don't care operations, allow it
	if((open || chdir) && !exec && !write && !read && !access && !append){
		permit = true;
	}

	// logging
	#ifdef DEBUG
	if(!permit){
		pr_info("mp4 not permitted: ncoaprwe [%d][%d][%d][%d][%d][%d][%d][%d] ssid [%d] osid [%d] mask [%d] is_dir [%d]\n",
			not_block, chdir, open, access, append, read, write, exec, ssid, osid, mask, is_dir);
	}
	#endif

	// return properly
	if(permit){
		return 0;
	}else{
		return -EACCES;
	}
}

/**
 * mp4_inode_permission - Check permission for an inode being opened
 *
 * @inode: the inode in question
 * @mask: the access requested
 *
 * This is the important access check hook
 *
 * returns 0 if access is granted, -EACCESS otherwise
 *
 */
static int mp4_inode_permission(struct inode *inode, int mask){
	const struct cred *this_cred;
	struct dentry *dent;

	char *path_buf;
	char *path_ptr;

	int object_flag;
	int subject_flag;
	int permission;

	// get dentry
	dent = d_find_alias(inode);
	if(dent == NULL){
		return 0;
	}

	// get path
	path_buf = (char*) kmalloc(MP4_PATH_BUF_SIZE, GFP_KERNEL);
	if(path_buf == NULL){
		dput(dent);
		return 0;
	}
	path_ptr = dentry_path_raw(dent, path_buf, MP4_PATH_BUF_SIZE);
	dput(dent);

	// test for skipping
	if(mp4_should_skip_path(path_ptr)){
		kfree(path_buf);
		return 0;
	}

	// subjective security id
	this_cred = current_cred();
	if(this_cred->security == NULL){
		subject_flag = MP4_NO_ACCESS;
	}else{
		subject_flag = SUBJECTIVE_BLOB(this_cred)->mp4_flags;
	}
	// objective security id
	object_flag = get_inode_sid(inode);
	// subroutine
	permission = mp4_has_permission(subject_flag, object_flag, mask, S_ISDIR(inode->i_mode));
	// logging
	if(permission != 0){
		pr_info("mp4_inode_permission denied [%s]\n", path_ptr);
	}

	// clean up
	kfree(path_buf);

	return permission;
}


/*
 * This is the list of hooks that we will using for our security module.
 */
static struct security_hook_list mp4_hooks[] = {
	// inode function to assign a label and to check permission
	LSM_HOOK_INIT(inode_init_security, mp4_inode_init_security),
	LSM_HOOK_INIT(inode_permission, mp4_inode_permission),
	
	// setting the credentials subjective security label when laucnhing a binary
	LSM_HOOK_INIT(bprm_set_creds, mp4_bprm_set_creds),

	// credentials handling and preparation 
	LSM_HOOK_INIT(cred_alloc_blank, mp4_cred_alloc_blank),
	LSM_HOOK_INIT(cred_free, mp4_cred_free),
	LSM_HOOK_INIT(cred_prepare, mp4_cred_prepare)
};

// init
static __init int mp4_init(void)
{
	// check if mp4 lsm is enabled with boot parameters
	if (!security_module_enable("mp4")){
		return 0;
	}

	pr_info("mp4 LSM initializing..");

	// Register the mp4 hooks with lsm
	security_add_hooks(mp4_hooks, ARRAY_SIZE(mp4_hooks));

	return 0;
}

/*
 * early registration with the kernel
 */
security_initcall(mp4_init);
