#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { CJSON_OBJECT = 1, CJSON_ARRAY, CJSON_STRING, CJSON_NUMBER, CJSON_BOOL, CJSON_NULL };
typedef struct { const char *p, *end; } Parser;

static void ws(Parser *p) { while (p->p < p->end && isspace((unsigned char)*p->p)) ++p->p; }
static cJSON *node(int type) { cJSON *n=calloc(1,sizeof(*n)); if(n)n->type=type; return n; }
static void add(cJSON *parent,cJSON *child) { if(!parent->child) parent->child=child; else { cJSON *p=parent->child; while(p->next)p=p->next; p->next=child; } }
static char *parse_string(Parser *p) {
    if(p->p>=p->end||*p->p!='"') return NULL;
    ++p->p;
    char *s=calloc((size_t)(p->end-p->p)+1,1); if(!s)return NULL; size_t n=0;
    while(p->p<p->end&&*p->p!='"') { unsigned char c=(unsigned char)*p->p++;
        if(c=='\\') { if(p->p>=p->end) { free(s); return NULL; } char e=*p->p++;
            switch(e) { case '"':case '\\':case '/':s[n++]=e;break;case 'n':s[n++]='\n';break;case 'r':s[n++]='\r';break;case 't':s[n++]='\t';break;case 'b':s[n++]='\b';break;case 'f':s[n++]='\f';break;case 'u':
                if(p->end-p->p<4) { free(s); return NULL; }
                if(p->p[0]=='0'&&p->p[1]=='0'&&p->p[2]=='0'&&p->p[3]=='0') s[n++]='\0'; else s[n++]='?'; p->p+=4; break;
                default: free(s);return NULL; }
        } else s[n++]=(char)c;
    }
    if(p->p>=p->end) { free(s); return NULL; } ++p->p; return s;
}
static cJSON *parse_value(Parser *p);
static cJSON *parse_compound(Parser *p,int type,char close) {
    cJSON *root=node(type); if(!root)return NULL; ++p->p; ws(p);
    if(p->p<p->end&&*p->p==close) { ++p->p; return root; }
    while(p->p<p->end) {
        char *key=NULL;
        if(type==CJSON_OBJECT) { key=parse_string(p); if(!key)goto fail; ws(p); if(p->p>=p->end||*p->p++!=':') { free(key);goto fail; } ws(p); }
        cJSON *v=parse_value(p); if(!v) { free(key);goto fail; } v->string=key; add(root,v); ws(p);
        if(p->p<p->end&&*p->p==close) { ++p->p; return root; }
        if(p->p>=p->end||*p->p++!=',')goto fail;
        ws(p);
    }
fail: cJSON_Delete(root); return NULL;
}
static cJSON *parse_value(Parser *p) {
    ws(p); if(p->p>=p->end)return NULL;
    if(*p->p=='{')return parse_compound(p,CJSON_OBJECT,'}');
    if(*p->p=='[')return parse_compound(p,CJSON_ARRAY,']');
    if(*p->p=='"') { cJSON *n=node(CJSON_STRING); if(n)n->valuestring=parse_string(p); if(!n||!n->valuestring){cJSON_Delete(n);return NULL;} return n; }
    if(*p->p=='-'||isdigit((unsigned char)*p->p)) { char *end=NULL; double d=strtod(p->p,&end); if(end==p->p||end>p->end)return NULL; cJSON*n=node(CJSON_NUMBER);if(n)n->valuedouble=d;p->p=end;return n; }
    if(p->end-p->p>=4&&memcmp(p->p,"true",4)==0){p->p+=4;return node(CJSON_BOOL);}
    if(p->end-p->p>=5&&memcmp(p->p,"false",5)==0){p->p+=5;return node(CJSON_BOOL);}
    if(p->end-p->p>=4&&memcmp(p->p,"null",4)==0){p->p+=4;return node(CJSON_NULL);}
    return NULL;
}
cJSON *cJSON_ParseWithLength(const char *text,size_t length) { Parser p={text,text+length}; cJSON*n=parse_value(&p);ws(&p);if(p.p!=p.end){cJSON_Delete(n);return NULL;}return n; }
void cJSON_Delete(cJSON *n) { if(!n)return; cJSON *c=n->child;while(c){cJSON*next=c->next;cJSON_Delete(c);c=next;}free(n->string);free(n->valuestring);free(n); }
int cJSON_IsObject(const cJSON*n){return n&&n->type==CJSON_OBJECT;}
int cJSON_IsArray(const cJSON*n){return n&&n->type==CJSON_ARRAY;}
int cJSON_IsString(const cJSON*n){return n&&n->type==CJSON_STRING;}
int cJSON_IsNumber(const cJSON*n){return n&&n->type==CJSON_NUMBER;}
const cJSON*cJSON_GetObjectItemCaseSensitive(const cJSON*n,const char*k){if(!n||!k)return NULL;for(cJSON*c=n->child;c;c=c->next)if(c->string&&strcmp(c->string,k)==0)return c;return NULL;}
const cJSON*cJSON_GetArrayItem(const cJSON*n,int i){if(!n||i<0)return NULL;cJSON*c=n->child;while(c&&i--)c=c->next;return c;}
int cJSON_GetArraySize(const cJSON*n){int z=0;if(n&&n->type==CJSON_ARRAY)for(cJSON*c=n->child;c;c=c->next)++z;return z;}
const char*cJSON_GetStringValue(const cJSON*n){return n&&n->type==CJSON_STRING?n->valuestring:NULL;}
