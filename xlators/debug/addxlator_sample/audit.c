    /* 
     This translator audits the unlink and rmdir operation by  
     the client. The operation can be logged at server logs  
     including client_id,file_id,operation , which guarantees 
     the security of the system. 
    */  
    #ifndef _CONFIG_H  
    #define _CONFIG_H  
    #include "config.h"  
    #include "xlator.h"  
    #endif  
      
    #include <fnmatch.h>  
    #include <errno.h>  
    #include "glusterfs.h"  
    #include "xlator.h"  
    #include <stdarg.h>  
    #include "defaults.h"  
    #include "logging.h"  
      
    /*unlink call back function*/  
    int   
    audit_unlink_cbk(call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret, int32_t op_errno, struct iatt *preparent, struct iatt *postparent, dict_t *xdata)  
    {  
        gf_log(this->name,GF_LOG_ERROR,"in audit translator unlink call back");  
          
        //client info: frame->root->client->client_uid  
        if(frame!=NULL&&frame->root!=NULL&&frame->root->client!=NULL&&frame->root->client->client_uid!=NULL )  
        {  
            gf_log(this->name,GF_LOG_INFO,"in audit translator unlink call back client = %s.opt=unlink",frame->root->client->client_uid);  
        }  
      
        STACK_UNWIND_STRICT(unlink,frame,op_ret,op_errno,preparent,postparent,xdata);  
        return 0;  
    }  
      
    /*rmdir call back function*/  
    int  
    audit_rmdir_cbk(call_frame_t *frame,void *cookie,xlator_t *this,int32_t op_ret, int32_t op_errno, struct iatt *preparent,struct iatt *postparent, dict_t *xdata)  
    {     
        gf_log(this->name,GF_LOG_ERROR,"in audit translator rmdir call back");  
      
        if(frame!=NULL&&frame->root!=NULL&&frame->root->client!=NULL&&frame->root->client->client_uid!=NULL )  
        {  
            gf_log(this->name,GF_LOG_ERROR,"in audit translator rmdir call back. client = %s.operation = rmdir",frame->root->client->client_uid);  
        }  
      
        STACK_UNWIND_STRICT(rmdir,frame,op_ret,op_errno,preparent,postparent,xdata);  
        return 0;  
    }  
      
    static int  
    audit_unlink(call_frame_t *frame, xlator_t *this, loc_t *loc, int xflag,dict_t *xdata)  
    {     
        struct ios_conf *conf = NULL;  
        conf = this->private;  
        gf_log(this->name,GF_LOG_INFO,"in audit unlink path=%s",loc->path);  
      
        STACK_WIND(frame,audit_unlink_cbk,FIRST_CHILD(this),FIRST_CHILD(this)->fops->unlink,loc,xflag,xdata);  
        return 0;  
    }  
      
    static int  
    audit_rmdir(call_frame_t *frame, xlator_t *this, loc_t *loc,int flags, dict_t *xdata)  
    {  
        gf_log(this->name,GF_LOG_INFO,"in audit translator rmdir.path= %s",loc->path);  
          
        STACK_WIND(frame,audit_rmdir_cbk,FIRST_CHILD(this),FIRST_CHILD(this)->fops->rmdir,loc,flags,xdata);  
        return 0;  
    }  
      
    int   
    reconfigure(xlator_t *this , dict_t *options)  
    {  
        return 0;  
    }  
      
    int   
    init(xlator_t *this)  
    {  
        struct ios_conf *conf = NULL;  
        int         ret = -1;  
        gf_log(this->name,GF_LOG_ERROR,"audit translator loaded");  
        if(!this)  
            return -1;  
        if(!this->children)  
        {  
            gf_log(this->name,GF_LOG_ERROR,"audit translator requires at least one subvolume");  
            return -1;  
        }  
        if(!this->parents)  
        {  
            gf_log(this->name,GF_LOG_ERROR,"dangling volume.check volfile ") ;  
        }  
        conf = this->private;  
        this->private = conf;  
        ret = 0;  
        return ret;  
    }  
      
    void   
    fini(xlator_t *this)  
    {  
        struct ios_conf *conf = NULL;  
        if(!this) return ;  
      
        conf = this->private;  
          
        if(!conf)  
            return ;  
          
        this->private = NULL;  
      
        GF_FREE(conf);  
        gf_log(this->name,GF_LOG_ERROR,"audit translator unloaded");  
        return ;  
    }  
      
    int   
    notify(xlator_t *this,int32_t event,void *data,...)  
    {  
        default_notify(this,event,data);  
        return 0;  
    }  
      
    struct xlator_fops fops =   
    {  
        .unlink = audit_unlink,   //audit unlink operation   
        .rmdir = audit_rmdir,     //audit rmdir operation  
    };  
      
    struct xlator_cbks cbks =   
    {};  
      
    struct volume_options options[] =   
    {};  
