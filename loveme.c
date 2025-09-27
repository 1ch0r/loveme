#define _POSIX_C_SOURCE 200809L
// devstral_agent.c — Lovable-style repo agent with controller loop + interactive prompts
// Build: gcc -std=c11 -O2 -Wall -Wextra -o devstral_agent devstral_agent.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#define PATH_MAX_LEN 4096
#define BUF_SIZE 8192
#define DEFAULT_MAX_TOTAL (2*1024*1024)
#define DEFAULT_MAX_FILE  (256*1024)
#define MAX_PLAN_FILES 256
#define MAX_LINE 4096

typedef struct {
    char workdir[PATH_MAX_LEN];
    char model[PATH_MAX_LEN];
    char cli[PATH_MAX_LEN];
    char mode[32];          // "overview", "edit", "agent"
    char focus_file[PATH_MAX_LEN];
    char test_cmd[PATH_MAX_LEN];
    size_t max_total;
    size_t max_file;
    size_t ctx_size;
    size_t n_predict;
    int apply_changes;
    int run_tests;
} Config;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static void die(const char *msg) { perror(msg); exit(1); }

// Buffer helpers
static void buffer_init(Buffer *b) {
    b->cap = BUF_SIZE;
    b->data = malloc(b->cap);
    if (!b->data) die("malloc");
    b->len = 0;
    b->data[0] = '\0';
}
static void buffer_free(Buffer *b) { free(b->data); b->data=NULL; b->len=b->cap=0; }
static void buffer_clear(Buffer *b) { b->len=0; b->data[0]='\0'; }
static void buffer_append(Buffer *b, const char *s) {
    size_t sl=strlen(s);
    if (b->len+sl+1>b->cap) {
        size_t newcap=(b->len+sl+1)*2;
        b->data=realloc(b->data,newcap);
        if(!b->data) die("realloc");
        b->cap=newcap;
    }
    memcpy(b->data+b->len,s,sl);
    b->len+=sl;
    b->data[b->len]='\0';
}
static void buffer_append_n(Buffer *b,const char*s,size_t n){
    if(b->len+n+1>b->cap){
        size_t newcap=(b->len+n+1)*2;
        b->data=realloc(b->data,newcap);
        if(!b->data) die("realloc");
        b->cap=newcap;
    }
    memcpy(b->data+b->len,s,n);
    b->len+=n;
    b->data[b->len]='\0';
}
static void buffer_append_fmt(Buffer *b,const char*fmt,...){
    va_list ap;va_start(ap,fmt);
    char tmp[BUF_SIZE];
    vsnprintf(tmp,sizeof(tmp),fmt,ap);
    va_end(ap);
    buffer_append(b,tmp);
}

// File helpers
static int ends_with(const char*s,const char*suf){
    size_t ls=strlen(s),lf=strlen(suf);
    return lf<=ls && strcmp(s+ls-lf,suf)==0;
}
static int is_code_file(const char*path){
    const char*exts[]={".c",".h",".cpp",".cc",".cxx",".py",".js",".ts",".jsx",".tsx",
        ".java",".cs",".go",".rs",".rb",".php",".swift",".kt",".m",".mm",".scala",".lua",".pl",".sh"};
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++)
        if(ends_with(path,exts[i])) return 1;
    return 0;
}
static void build_repo_index(const char*root,Buffer*ctx){
    buffer_append_fmt(ctx,"Repository root: %s\n",root);
    DIR*d=opendir(root); if(!d) return;
    struct dirent*ent;
    while((ent=readdir(d))){
        if(ent->d_name[0]=='.') continue;
        char full[PATH_MAX_LEN];
        snprintf(full,sizeof(full),"%s/%s",root,ent->d_name);
        struct stat st; if(stat(full,&st)!=0) continue;
        if(S_ISREG(st.st_mode)&&is_code_file(ent->d_name)){
            buffer_append_fmt(ctx,"- %s (%lld bytes)\n",ent->d_name,(long long)st.st_size);
        }
    }
    closedir(d);
}
static void load_file(const char*path,Buffer*ctx,size_t max_file){
    FILE*f=fopen(path,"r"); if(!f){buffer_append_fmt(ctx,"[Error] cannot read %s\n",path);return;}
    buffer_append_fmt(ctx,"\n--- %s ---\n",path);
    char buf[4096]; size_t total=0,n;
    while((n=fread(buf,1,sizeof(buf),f))>0 && total<max_file){
        size_t tocopy=n; if(total+tocopy>max_file) tocopy=max_file-total;
        buffer_append_n(ctx,buf,tocopy); total+=tocopy;
    }
    fclose(f);
}

// Prompt building
static void append_system(Buffer*out){
    buffer_append(out,
"<|system|>\n"
"You are Devstral, a Lovable/OpenHands-style repository agent.\n"
"- First produce a clear plan when asked to analyze.\n"
"- For edits, output changes strictly in the following format per file:\n"
"  <<<FILE: relative/path>>>\n"
"  <<<REPLACEMENT_START>>>\n"
"  <entire new file content>\n"
"  <<<REPLACEMENT_END>>>\n"
"- Do NOT include prose between file blocks.\n"
"- Prefer complete file replacements.\n\n");
}
static void build_overview_prompt(const Config*cfg,const char*task,Buffer*out){
    append_system(out);
    Buffer repo; buffer_init(&repo);
    build_repo_index(cfg->workdir,&repo);
    buffer_append(out,"<|repo_index|>\n"); buffer_append(out,repo.data);
    buffer_append(out,"\n<|user|>\n"); buffer_append_fmt(out,"TASK: %s\n",task);
    buffer_free(&repo);
}
static void build_edit_prompt(const Config*cfg,const char*task,const char*rel_file,Buffer*out){
    append_system(out);
    Buffer repo; buffer_init(&repo);
    build_repo_index(cfg->workdir,&repo);
    buffer_append(out,"<|repo_index|>\n"); buffer_append(out,repo.data);
    char full[PATH_MAX_LEN]; snprintf(full,sizeof(full),"%s/%s",cfg->workdir,rel_file);
    Buffer file; buffer_init(&file); load_file(full,&file,cfg->max_file);
    buffer_append(out,"\n<|focus_file|>\n"); buffer_append(out,file.data);
    buffer_append(out,"\n<|user|>\n");
    buffer_append_fmt(out,"TASK: Apply edits to %s. Instruction: %s\n",rel_file,task);
    buffer_free(&file); buffer_free(&repo);
}

// Run llama.cpp
static int run_llama(const Config*cfg,const char*prompt,Buffer*out){
    char tmpfile[PATH_MAX_LEN]; snprintf(tmpfile,sizeof(tmpfile),"/tmp/devstral_prompt_%u.txt",(unsigned)time(NULL));
    FILE*f=fopen(tmpfile,"w"); if(!f) die("tmpfile"); fputs(prompt,f); fclose(f);
    char cmd[PATH_MAX_LEN*4];
    snprintf(cmd,sizeof(cmd),"%s -m %s -c %zu -n %zu -p \"$(cat %s)\"",
             cfg->cli,cfg->model,cfg->ctx_size,cfg->n_predict,tmpfile);
    fprintf(stderr,"[Running] %s\n",cmd);
    FILE*pipe=popen(cmd,"r"); if(!pipe) die("popen");
    buffer_clear(out);
    char buf[4096]; size_t n;
    while((n=fread(buf,1,sizeof(buf),pipe))>0){ buffer_append_n(out,buf,n); fwrite(buf,1,n,stdout); fflush(stdout);}
    int rc=pclose(pipe); unlink(tmpfile); return rc;
}

// Utilities
static void trim(char*s){
    size_t len=strlen(s);
    size_t i=0; while(i<len && isspace((unsigned char)s[i])) i++;
    size_t j=len; while(j>i && isspace((unsigned char)s[j-1])) j--;
    if(i>0 || j<len) memmove(s, s+i, j-i);
    s[j-i] = '\0';
}

// Parse plan: looks for lines naming files
static size_t parse_plan_files(const char *text, char files[][PATH_MAX_LEN], size_t max_files){
    size_t count=0;
    char *copy=strdup(text); if(!copy) die("strdup");
    char *line=strtok(copy,"\n");
    while(line && count<max_files){
        char lbuf[MAX_LINE]; snprintf(lbuf,sizeof(lbuf),"%s",line);
        trim(lbuf);
        if(!*lbuf){ line=strtok(NULL,"\n"); continue; }
        // Accept formats:
        // - path/to/file.ext
        // FILE: path/to/file.ext
        // FILES: a, b, c
        if (strncmp(lbuf,"FILES:",6)==0) {
            char *p=lbuf+6; trim(p);
            char *tok=strtok(p,",");
            while(tok && count<max_files){
                trim(tok);
                if(*tok) snprintf(files[count++],PATH_MAX_LEN,"%s",tok);
                tok=strtok(NULL,",");
            }
        } else if (strncmp(lbuf,"FILE:",5)==0) {
            char *p=lbuf+5; trim(p);
            if(*p) snprintf(files[count++],PATH_MAX_LEN,"%s",p);
        } else if (lbuf[0]=='-' || lbuf[0]=='*') {
            char *p=lbuf+1; trim(p);
            if(*p && is_code_file(p)) snprintf(files[count++],PATH_MAX_LEN,"%s",p);
        } else if (is_code_file(lbuf)) {
            snprintf(files[count++],PATH_MAX_LEN,"%s",lbuf);
        }
        line=strtok(NULL,"\n");
    }
    free(copy);
    return count;
}

// Apply replacement blocks:
// <<<FILE: relative/path>>>
// <<<REPLACEMENT_START>>>
// <content>
// <<<REPLACEMENT_END>>>
static int apply_replacements(const Config*cfg,const char*model_out){
    const char*p=model_out;int applied=0;
    while(1){
        const char*tag=strstr(p,"<<<FILE:"); if(!tag) break;
        const char*tag_end=strstr(tag,">>>"); if(!tag_end) break;
        char path[PATH_MAX_LEN]; memset(path,0,sizeof(path));
        size_t plen=(size_t)(tag_end - (tag+8));
        if(plen>=PATH_MAX_LEN) plen=PATH_MAX_LEN-1;
        strncpy(path, tag+8, plen); path[plen]='\0'; trim(path);

        const char*start_tag=strstr(tag_end,"<<<REPLACEMENT_START>>>"); if(!start_tag) break;
        start_tag += strlen("<<<REPLACEMENT_START>>>");
        const char*end_tag=strstr(start_tag,"<<<REPLACEMENT_END>>>"); if(!end_tag) break;

        size_t content_len=(size_t)(end_tag - start_tag);
        char *content=(char*)malloc(content_len+1); if(!content) die("malloc");
        memcpy(content,start_tag,content_len); content[content_len]='\0';

        char full[PATH_MAX_LEN]; snprintf(full,sizeof(full),"%s/%s",cfg->workdir,path);
        if(cfg->apply_changes){
            FILE *f=fopen(full,"w");
            if(!f){
                fprintf(stderr,"[Apply] Failed to write %s (%s)\n", full, strerror(errno));
            } else {
                fwrite(content,1,content_len,f); fclose(f);
                fprintf(stderr,"[Apply] Replaced %s\n", path); applied++;
            }
        } else {
            fprintf(stderr,"[Dry-run] Would replace %s\n", path);
        }

        free(content);
        p = end_tag + strlen("<<<REPLACEMENT_END>>>");
    }
    return applied;
}

// Run shell command
static int run_shell(const char *cmd){
    fprintf(stderr,"[Shell] %s\n", cmd);
    int rc=system(cmd);
    if(rc==-1) fprintf(stderr,"[Shell] system() failed: %s\n", strerror(errno));
    else fprintf(stderr,"[Shell] exit code: %d\n", WEXITSTATUS(rc));
    return rc;
}

// Agent controller: plan → edit → apply → test
static void build_overview_prompt(const Config*cfg,const char*task,Buffer*out);
static void build_edit_prompt(const Config*cfg,const char*task,const char*rel_file,Buffer*out);

static void agent_controller(const Config *cfg, const char *task){
    Buffer prompt; buffer_init(&prompt);
    Buffer out; buffer_init(&out);

    // Phase 1: planning
    buffer_clear(&prompt);
    build_overview_prompt(cfg, task, &prompt);
    fprintf(stderr,"\n[Agent] Planning...\n");
    run_llama(cfg, prompt.data, &out);

    // Parse plan
    char plan_files[MAX_PLAN_FILES][PATH_MAX_LEN];
    size_t nfiles = parse_plan_files(out.data, plan_files, MAX_PLAN_FILES);
    if(nfiles==0){
        fprintf(stderr,"[Agent] No files parsed from plan. You may specify --mode edit --file <path>.\n");
    } else {
        fprintf(stderr,"[Agent] Plan selected %zu file(s)\n", nfiles);
    }

    // Phase 2: per-file edits
    for(size_t i=0;i<nfiles;i++){
        const char *rel=plan_files[i];
        fprintf(stderr,"\n[Agent] Editing %s...\n", rel);
        buffer_clear(&prompt);
        build_edit_prompt(cfg, task, rel, &prompt);
        buffer_clear(&out);
        run_llama(cfg, prompt.data, &out);
        int applied = apply_replacements(cfg, out.data);
        if(applied==0){
            fprintf(stderr,"[Agent] No valid replacement blocks for %s\n", rel);
        }
    }

    // Phase 3: tests
    if(cfg->run_tests && cfg->test_cmd[0]){
        char cmdline[PATH_MAX_LEN*2];
        snprintf(cmdline,sizeof(cmdline),"cd %s && %s", cfg->workdir, cfg->test_cmd);
        run_shell(cmdline);
    }

    buffer_free(&prompt);
    buffer_free(&out);
}

// Interactive prompt helper
static void prompt_user(const char*label,char*out,size_t outlen,const char*def){
    printf("%s [%s]: ",label,def?def:""); fflush(stdout);
    char buf[PATH_MAX_LEN];
    if(fgets(buf,sizeof(buf),stdin)){
        buf[strcspn(buf,"\n")]=0;
        if(strlen(buf)>0){snprintf(out,outlen,"%s",buf);return;}
    }
    if(def) snprintf(out,outlen,"%s",def); else out[0]='\0';
}

int main(int argc,char**argv){
    Config cfg={0};
    cfg.max_total=DEFAULT_MAX_TOTAL;
    cfg.max_file=DEFAULT_MAX_FILE;
    cfg.ctx_size=32768;
    cfg.n_predict=2048;
    strcpy(cfg.mode,"overview");
    cfg.apply_changes=0;
    cfg.run_tests=0;
    const char*task=NULL;

    // Parse CLI args
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--workdir")==0 && i+1<argc) snprintf(cfg.workdir,sizeof(cfg.workdir),"%s",argv[++i]);
        else if(strcmp(argv[i],"--model")==0 && i+1<argc) snprintf(cfg.model,sizeof(cfg.model),"%s",argv[++i]);
        else if(strcmp(argv[i],"--cli")==0 && i+1<argc) snprintf(cfg.cli,sizeof(cfg.cli),"%s",argv[++i]);
        else if(strcmp(argv[i],"--task")==0 && i+1<argc) task=argv[++i];
        else if(strcmp(argv[i],"--ctx")==0 && i+1<argc) cfg.ctx_size=strtoull(argv[++i],NULL,10);
        else if(strcmp(argv[i],"--n-predict")==0 && i+1<argc) cfg.n_predict=strtoull(argv[++i],NULL,10);
        else if(strcmp(argv[i],"--mode")==0 && i+1<argc) snprintf(cfg.mode,sizeof(cfg.mode),"%s",argv[++i]);
        else if(strcmp(argv[i],"--file")==0 && i+1<argc) snprintf(cfg.focus_file,sizeof(cfg.focus_file),"%s",argv[++i]);
        else if(strcmp(argv[i],"--apply")==0) cfg.apply_changes=1;
        else if(strcmp(argv[i],"--run-tests")==0) cfg.run_tests=1;
        else if(strcmp(argv[i],"--test-cmd")==0 && i+1<argc) snprintf(cfg.test_cmd,sizeof(cfg.test_cmd),"%s",argv[++i]);
    }

    // Interactive prompts if missing
    if(!cfg.workdir[0]) prompt_user("Repository workdir",cfg.workdir,sizeof(cfg.workdir),".");
    if(!cfg.model[0]) prompt_user("Model path (.gguf)",cfg.model,sizeof(cfg.model),"model.gguf");
    if(!cfg.cli[0]) prompt_user("llama.cpp CLI path",cfg.cli,sizeof(cfg.cli),"./llama-cli");

    char taskbuf[2048];
    if(!task){ prompt_user("Task/Instruction",taskbuf,sizeof(taskbuf),"Refactor code for clarity"); task=taskbuf; }

    char modebuf[32];
    if(strcmp(cfg.mode,"overview")!=0 && strcmp(cfg.mode,"edit")!=0 && strcmp(cfg.mode,"agent")!=0){
        prompt_user("Mode (overview/edit/agent)",modebuf,sizeof(modebuf),"agent");
        snprintf(cfg.mode,sizeof(cfg.mode),"%s",modebuf);
    }

    if(strcmp(cfg.mode,"edit")==0 && !cfg.focus_file[0]){
        prompt_user("File to edit (relative path)",cfg.focus_file,sizeof(cfg.focus_file),"main.c");
    }

    char yn[8];
    if(!cfg.apply_changes){
        prompt_user("Apply changes to disk? (y/n)",yn,sizeof(yn),"n");
        if(yn[0]=='y'||yn[0]=='Y') cfg.apply_changes=1;
    }
    if(!cfg.run_tests){
        prompt_user("Run tests after edits? (y/n)",yn,sizeof(yn),"n");
        if(yn[0]=='y'||yn[0]=='Y'){
            cfg.run_tests=1;
            if(!cfg.test_cmd[0]) prompt_user("Test command",cfg.test_cmd,sizeof(cfg.test_cmd),"make test");
        }
    }

    // Dispatch
    if(strcmp(cfg.mode,"agent")==0){
        agent_controller(&cfg,task);
        return 0;
    }

    Buffer prompt; buffer_init(&prompt);
    Buffer out; buffer_init(&out);

    if(strcmp(cfg.mode,"overview")==0){
        build_overview_prompt(&cfg,task,&prompt);
    } else if(strcmp(cfg.mode,"edit")==0 && cfg.focus_file[0]){
        build_edit_prompt(&cfg,task,cfg.focus_file,&prompt);
    } else {
        fprintf(stderr,"[Error] Unknown mode or missing --file for edit.\n");
        buffer_free(&prompt); buffer_free(&out);
        return 1;
    }

    run_llama(&cfg,prompt.data,&out);

    if(strcmp(cfg.mode,"edit")==0){
        apply_replacements(&cfg,out.data);
        if(cfg.run_tests && cfg.test_cmd[0]){
            char cmdline[PATH_MAX_LEN*2];
            snprintf(cmdline,sizeof(cmdline),"cd %s && %s",cfg.workdir,cfg.test_cmd);
            run_shell(cmdline);
        }
    }

    buffer_free(&prompt);
    buffer_free(&out);
    return 0;
}

