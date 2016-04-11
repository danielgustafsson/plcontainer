#include <assert.h>

#include <errno.h>
#include <netinet/ip.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>

#include <R.h>
#include <Rversion.h>
#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>
#include <Rdefines.h>

#if (R_VERSION >= 132352) /* R_VERSION >= 2.5.0 */
#define R_PARSEVECTOR(a_, b_, c_)               R_ParseVector(a_, b_, (ParseStatus *) c_, R_NilValue)
#else /* R_VERSION < 2.5.0 */
#define R_PARSEVECTOR(a_, b_, c_)               R_ParseVector(a_, b_, (ParseStatus *) c_)
#endif /* R_VERSION >= 2.5.0 */


/* R's definition conflicts with the ones defined by postgres */
#undef WARNING
#undef ERROR

#include "common/comm_channel.h"
#include "common/comm_utils.h"
#include "common/comm_connectivity.h"
#include "common/comm_server.h"
#include "rcall.h"

static SEXP coerce_to_char(SEXP rval);
SEXP convert_args(callreq req);
static void
pg_get_one_r(char *value,  plcDatatype column_type, SEXP *obj, int elnum);
static void
pg_get_null( plcDatatype column_type, SEXP *obj, int elnum);

SEXP get_r_vector(plcDatatype type_id, int numels);
int get_entry_length(plcDatatype type);

#define OPTIONS_NULL_CMD    "options(error = expression(NULL))"

/* install the error handler to call our throw_r_error */
#define THROWRERROR_CMD \
            "pg.throwrerror <-function(msg) " \
            "{" \
            "  msglen <- nchar(msg);" \
            "  if (substr(msg, msglen, msglen + 1) == \"\\n\")" \
            "    msg <- substr(msg, 1, msglen - 1);" \
            "  .C(\"throw_r_error\", as.character(msg));" \
            "}"
#define OPTIONS_THROWRERROR_CMD \
            "options(error = expression(pg.throwrerror(geterrmessage())))"

/* install the notice handler to call our throw_r_notice */
#define THROWNOTICE_CMD \
            "pg.thrownotice <-function(msg) " \
            "{.C(\"throw_pg_notice\", as.character(msg))}"
#define THROWERROR_CMD \
            "pg.throwerror <-function(msg) " \
            "{stop(msg, call. = FALSE)}"
#define OPTIONS_THROWWARN_CMD \
            "options(warning.expression = expression(pg.thrownotice(last.warning)))"

#define QUOTE_LITERAL_CMD \
            "pg.quoteliteral <-function(sql) " \
            "{.Call(\"plr_quote_literal\", sql)}"
#define QUOTE_IDENT_CMD \
            "pg.quoteident <-function(sql) " \
            "{.Call(\"plr_quote_ident\", sql)}"
#define SPI_EXEC_CMD \
            "pg.spi.exec <-function(sql) {.Call(\"plr_SPI_exec\", sql)}"

#define SPI_DBGETQUERY_CMD \
            "dbGetQuery <-function(sql) {\n" \
            "data <- pg.spi.exec(sql)\n" \
            "return(data)\n" \
            "}"

int R_SignalHandlers = 1;  /* Exposed in R_interface.h */

static void load_r_cmd(const char *cmd);
static char * get_load_self_ref_cmd(const char *libstr);

// Initialization of R module
void r_init( );


/*
  based on examples from:
  1. https://github.com/parkerabercrombie/call-r-from-c/blob/master/r_test.c
  2. https://github.com/wch/r-source/tree/trunk/tests/Embedding
  3. http://pabercrombie.com/wordpress/2014/05/how-to-call-an-r-function-from-c/

  Other resources:
  - https://cran.r-project.org/doc/manuals/r-release/R-exts.html
  - http://adv-r.had.co.nz/C-interface.html
 */

static void send_error(plcConn* conn, char *msg);
static char * create_r_func(callreq req);

static char *create_r_func(callreq req);
static SEXP parse_r_code(const char *code, plcConn* conn, int *errorOccurred);

static plcIterator *matrix_iterator(SEXP mtx, plcDatatype type);
static void matrix_iterator_free(plcIterator *iter);

/*
 * set by hook throw_r_error
 */
char *last_R_error_msg,
     *last_R_notice;

extern SEXP plr_SPI_execp(const char * sql);



plcConn* plcconn;


void r_init( ) {
    char   *argv[] = {"client", "--slave", "--vanilla"};
    char   *    buf;

    /*
     * Stop R using its own signal handlers Otherwise, R will prompt the user for what to do and
         will hang in the container
    */
    R_SignalHandlers = 0;

    if( !Rf_initEmbeddedR(sizeof(argv) / sizeof(*argv), argv) ){
        //TODO: return an error
        ;
    }



    /*
     * temporarily turn off R error reporting -- it will be turned back on
     * once the custom R error handler is installed from the plr library
     */
    load_r_cmd(OPTIONS_NULL_CMD);

    /* next load the plr library into R */
    load_r_cmd(buf=get_load_self_ref_cmd("librcall.so"));
    pfree(buf);

    load_r_cmd(THROWRERROR_CMD);
    load_r_cmd(OPTIONS_THROWRERROR_CMD);
    load_r_cmd(THROWNOTICE_CMD);
    load_r_cmd(THROWERROR_CMD);
    load_r_cmd(OPTIONS_THROWWARN_CMD);
    load_r_cmd(QUOTE_LITERAL_CMD);
    load_r_cmd(QUOTE_IDENT_CMD);
    load_r_cmd(SPI_EXEC_CMD);
    load_r_cmd(SPI_DBGETQUERY_CMD);
}

static  char *get_load_self_ref_cmd(const char *libstr)
{
    char   *buf =  (char *) pmalloc(strlen(libstr) + 12 + 1);;

    sprintf(buf, "dyn.load(\"%s\")", libstr);
    return buf;
}

static void
load_r_cmd(const char *cmd)
{
    SEXP        cmdSexp,
                cmdexpr;
    int            i,
                status=0;


    PROTECT(cmdSexp = NEW_CHARACTER(1));
    SET_STRING_ELT(cmdSexp, 0, COPY_TO_USER_STRING(cmd));
    PROTECT(cmdexpr = R_PARSEVECTOR(cmdSexp, -1, &status));
    if (status != PARSE_OK) {
        UNPROTECT(2);
        goto error;
    }

    /* Loop is needed here as EXPSEXP may be of length > 1 */
    for(i = 0; i < length(cmdexpr); i++)
    {
        R_tryEval(VECTOR_ELT(cmdexpr, i), R_GlobalEnv, &status);
        if(status != 0)
        {
            goto error;
        }
    }

    UNPROTECT(2);
    return;

error:
    // TODO send error back to client
    printf("Error loading %s \n ",cmd);
    return;

}
#ifdef XXX
static plcDatatype get_base_type(SEXP rval)
{
    switch (TYPEOF(rval)) {
        case INTSXP:
            return PLC_DATA_INT8;
        case REALSXP:
            return PLC_DATA_FLOAT8;
        case STRSXP:
        default:
            return PLC_DATA_TEXT;
    }
}
#endif

void handle_call(callreq req, plcConn* conn) {
    SEXP             r,
                     dfcol,
                     strres,
                     call,
                     rargs,
                     obj,
                     args;

    int              i,
                     errorOccurred;

    char            *func,
                    *errmsg;

    const char 		*value;

    plcontainer_result res;

    /*
     * Keep our connection for future calls from R back to us.
    */
    plcconn = conn;

    /* wrap the input in a function and evaluate the result */
    func = create_r_func(req);

    PROTECT(r = parse_r_code(func, conn, &errorOccurred));

    pfree(func);
    if (errorOccurred) {
        //TODO send real error message
        /* run_r_code will send an error back */
        UNPROTECT(1); //r
        return;
    }

    if(req->nargs > 0)
    {
        rargs = convert_args(req);
        PROTECT(obj = args = allocList(req->nargs));

        for (i = 0; i < req->nargs; i++)
        {
            SETCAR(obj, VECTOR_ELT(rargs, i));
            obj = CDR(obj);
        }
        UNPROTECT(1);
        PROTECT(call = lcons(r, args));
    }
    else
    {
        PROTECT(call = allocVector(LANGSXP,1));
        SETCAR(call, r);
    }

    strres = R_tryEval(call, R_GlobalEnv, &errorOccurred);
    UNPROTECT(1); //call


    if (errorOccurred) {
        UNPROTECT(1); //r
        //TODO send real error message
        if (last_R_error_msg){
            errmsg = strdup(last_R_error_msg);
        }else{
            errmsg = strdup("Error executing\n");
            errmsg = realloc(errmsg, strlen(errmsg)+strlen(req->proc.src));
            errmsg = strcat(errmsg, req->proc.src);
        }
        send_error(conn, errmsg);
        free(errmsg);
        return;
    }

    if (isFrame(strres)) {
        SEXP names;
        PROTECT(names = getAttrib(strres,R_NamesSymbol));
        int cols;
        int rows, col, row;

        cols = length(strres);

        /* allocate a result */
        res          = pmalloc(sizeof(*res));
        res->msgtype = MT_RESULT;
        res->types   = pmalloc(sizeof(*res->types)*cols);
        res->names   = pmalloc(sizeof(*res->names)*cols);

        for (col = 0; col < cols; col++) {

            res->names[col] = pstrdup(CHAR(STRING_ELT(names,col)));
            res->types[col].type = PLC_DATA_TEXT;

            if (TYPEOF(strres) == VECSXP) {
                PROTECT(dfcol = VECTOR_ELT(strres, col));
            } else if (TYPEOF(strres) == LISTSXP) {
                PROTECT(dfcol = CAR(strres));
                strres = CDR(strres);
            } else {
                errmsg = strdup("plc_r: bad internal representation of data.frame");
                send_error(conn, errmsg);
                free(errmsg);
            }

            if (ATTRIB(dfcol) == R_NilValue ||
                TYPEOF(CAR(ATTRIB(dfcol))) != STRSXP){
                    PROTECT(obj = coerce_to_char(dfcol));
            }else{
                PROTECT(obj = coerce_to_char(CAR(ATTRIB(dfcol))));
            }

            /*
             * get the first column just to get the number of rows
             */
            if (col == 0) {
                rows = length(obj);

                res->data    = pmalloc(sizeof(*res->data) * rows);
                /*
                 * allocate memory when we do the first column
                 */
                for (row = 0; row < rows; row++) {
                    res->data[row] = pmalloc(cols * sizeof(res->data[0][0]));
                }
                res->rows = rows;
                res->cols = cols;
            }

            for (row = 0; row < rows; row++) {

                value = strdup(CHAR(STRING_ELT(obj, row)));

                if (STRING_ELT(obj, row) == NA_STRING || value == NULL)
                {
                    res->data[row][col].isnull  = true;
                    res->data[row][col].value   = NULL;
                }
                else
                {
                    res->data[row][col].isnull  = false;
                    res->data[row][col].value   = (char *)value;
                }
            }
            UNPROTECT(2);

        }
        UNPROTECT(1);
    } else {
        /* this is not a data frame */
        res          = pmalloc(sizeof(*res));
        res->msgtype = MT_RESULT;
        res->types   = pmalloc(sizeof(*res->types));
        res->names   = pmalloc(sizeof(*res->names));
        res->data    = pmalloc(sizeof(*res->data));
        res->data[0] = pmalloc(sizeof(*res->data[0]));
        res->rows = res->cols = 1;
        res->names[0]         = pstrdup("result");
        res->data[0]->isnull  = false;
        res->types[0].nSubTypes = 0;

        if ( (isMatrix(strres) || (isVector(strres) && length(strres) > 1)) && req->retType.type != PLC_DATA_TEXT ) {
            plcDatatype basetype = req->retType.subTypes[0].type;
            if (basetype > PLC_DATA_TEXT) {
                char *errmsg = pmalloc(100);
                sprintf(errmsg,
                        "Matrices of the type '%d' are not supported yet",
                        TYPEOF(strres));
                send_error(conn, errmsg);
                pfree(errmsg);
            }
            res->types[0].type         =  PLC_DATA_ARRAY;
            res->types[0].nSubTypes    = 1;
            res->types[0].subTypes =  pmalloc(sizeof (plcType) );
            res->types[0].subTypes->type = basetype;
            res->types[0].subTypes->nSubTypes = 0;

            res->data[0]->value   = (char*)matrix_iterator(strres, basetype);
        } else {
        	char *ret = NULL;
            switch(res->types[0].type = req->retType.type){
            	case PLC_DATA_INT1:
                    ret = pmalloc(1);
                    *((bool *)ret) = asLogical(strres);
            		break;
            	case PLC_DATA_INT2:
                    ret = (char *)pmalloc(sizeof(int16));
                    *((int16 *)ret) = asInteger(strres);
            		break;
            	case PLC_DATA_INT4:
                    ret = (char *)pmalloc(sizeof(int32));
                    *((int32 *)ret) = asInteger(strres);
            		break;
            	case PLC_DATA_INT8:
                    ret = (char *)pmalloc(sizeof(int64));
                    *((int64 *)ret) = asInteger(strres);
            		break;
            	case PLC_DATA_FLOAT4:
                    ret = (char *)pmalloc(sizeof(float4));
                    *((float4 *)ret) = (float4)asReal(strres);
                    break;
            	case PLC_DATA_FLOAT8:
                    ret = (char *)pmalloc(sizeof(float8));
                    *((float8 *)ret) = asReal(strres);
                    break;
            	case PLC_DATA_TEXT:
                    ret = strdup(CHAR(asChar(strres)));
                    break;
            	case PLC_DATA_ARRAY:
            	case PLC_DATA_RECORD:
            	case PLC_DATA_UDT:
                default:
                    res->data[0]->value   = pstrdup("NOT IMPLEMENTED");
                    break;
            }
            res->data[0]->value = ret;

        }
    }

    /* send the result back */
    plcontainer_channel_send(conn, (message)res);

    /* free the result object and the R values */
    free_result(res);

    UNPROTECT(1);

    return;
}


rawdata *matrix_iterator_next (plcIterator *iter) {
    plcArrayMeta *meta;
    int     *position;
    SEXP     mtx;
    rawdata *res;

    meta = (plcArrayMeta*)iter->meta;
    position = (int*)iter->position;
    mtx = (SEXP)iter->data;
    res = pmalloc(sizeof(rawdata));

    //lprintf(WARNING, "Position: %d, %d", position[0], position[1]);
    int idx=0;
    if (meta->ndims == 1){
		idx = position[0];
	}else if (meta->ndims == 2 ){
		idx = position[1]*meta->dims[0] + position[0];
	}else if (meta->ndims == 3) {
		lprintf(ERROR, "need to deal with 3 dimensions rcall.c");
	}

	if ( 1== 0 ){ //TYPEOF(((SEXP *)mtx)[idx]) == NILSXP ){
		res->isnull = TRUE;
		res->value  = NULL;
	}else {
		res->isnull = FALSE;

		switch (meta->type)
		{
			case PLC_DATA_INT2:
			case PLC_DATA_INT4:
				/* 2 and 4 byte integer pgsql datatype => use R INTEGER */
				res->value = pmalloc(4);
				*((int *)res->value) = INTEGER_DATA(mtx)[idx];
				break;

				/*
				 * Other numeric types => use R REAL
				 * Note pgsql int8 is mapped to R REAL
				 * because R INTEGER is only 4 byte
				 */

			case PLC_DATA_INT8:
				res->value = pmalloc(8);
				*((int64 *)res->value) = (int64)(NUMERIC_DATA(mtx)[idx]);
				break;

			case PLC_DATA_FLOAT4:
				res->value = pmalloc(4);
				*((float4 *)res->value) = (float4)(NUMERIC_DATA(mtx)[idx]);

				break;
			case PLC_DATA_FLOAT8:
				res->value = pmalloc(8);
				*((float8 *)res->value) = (float8)(NUMERIC_DATA(mtx)[idx]);

				break;
			case PLC_DATA_INT1:
				res->value = pmalloc(1);
				*((int *)res->value) = LOGICAL_DATA(mtx)[idx];
				break;
			case PLC_DATA_RECORD:
			case PLC_DATA_UDT:
			case PLC_DATA_INVALID:
			case PLC_DATA_ARRAY:
				lprintf(ERROR, "un-handled type %d", meta->type);
				break;
			case PLC_DATA_TEXT:

			    if (STRING_ELT(mtx, idx) != NA_STRING){
			        res->isnull = FALSE;
			        res->value  = pstrdup((char *) CHAR( STRING_ELT(mtx, idx) ));
			    } else {
			        res->isnull = TRUE;
			        res->value  = NULL;
			    }

			default:
				/* Everything else is defaulted to string */
				;//SET_STRING_ELT(*obj, elnum, COPY_TO_USER_STRING(value));
		}
	}

	if ( meta->ndims == 1){
		position[0] += 1;
	}else if (meta->ndims == 2){

		position[1] += 1;
		if (position[1] == meta->dims[1]) {
			position[1] = 0;
			position[0] += 1;
		}
	}else if (meta->ndims == 3) {
		lprintf(ERROR, "need to support 3 dimensions");
	}


    return res;
}

static plcIterator *matrix_iterator(SEXP mtx,plcDatatype base_type) {
    plcArrayMeta *meta;
    int *position, i;
    plcIterator *iter;

    /* Allocate the iterator */
    iter = (plcIterator*)pmalloc(sizeof(plcIterator));

    /* Initialize meta */
    meta = (plcArrayMeta*)pmalloc(sizeof(plcArrayMeta));
    meta->type = base_type;
    meta->ndims = 1;
    meta->dims  = (int*)pmalloc(sizeof(int));
    /*
     * R stores matrix in columns
     */
    meta->dims[0] = length(mtx);
    meta->size=1;
    for ( i=0; i< meta->ndims; i++){
    	meta->size *= meta->dims[i];
    }
    iter->meta = meta;

    /* Initializing initial position */
    position = (int*)pmalloc( meta->ndims * sizeof(int));

    for ( i=0; i< meta->ndims; i++){
    	position[i] = 0;
    }
    iter->position = (char*)position;

    /* Initializing "data" */
    iter->data = (char *)mtx;

    /* Initializing "next" function */
    iter->next = matrix_iterator_next;
    iter->cleanup = matrix_iterator_free;

    return iter;
}

static void matrix_iterator_free(plcIterator *iter) {
    pfree(((plcArrayMeta*)iter->meta)->dims);
    pfree(iter->meta);
    pfree(iter->position);
    UNPROTECT(1);
}

static void send_error(plcConn* conn, char *msg) {
    /* an exception was thrown */
    error_message err;
    err             = pmalloc(sizeof(*err));
    err->msgtype    = MT_EXCEPTION;
    err->message    = msg;
    err->stacktrace = "";

    /* send the result back */
    plcontainer_channel_send(conn, (message)err);

    /* free the objects */
    free(err);
}

static SEXP parse_r_code(const char *code,  plcConn* conn, int *errorOccurred) {
    /* int hadError; */
    ParseStatus status;
    char *      errmsg;
    SEXP        tmp,
                rbody,
                fun;

    PROTECT(rbody = mkString(code));
    /*
      limit the number of expressions to be parsed to 2:
        - the definition of the function, i.e. f <- function() {...}
        - the call to the function f()

      kind of useful to prevent injection, but pointless since we are
      running in a container. I think -1 is equivalent to no limit.
    */
    PROTECT(tmp = R_ParseVector(rbody, -1, &status, R_NilValue));

    if (tmp != R_NilValue){
        PROTECT(fun = VECTOR_ELT(tmp, 0));
    }else{
        PROTECT(fun = R_NilValue);
    }

    if (status != PARSE_OK) {
        UNPROTECT(3);
        if (last_R_error_msg != NULL){
            errmsg  = strdup(last_R_error_msg);
        }else{
            errmsg =  strdup("Parse Error\n");
            errmsg =  realloc(errmsg, strlen(errmsg)+strlen(code));
            errmsg =  strcat(errmsg, code);
        }
        goto error;
    }

    UNPROTECT(3);
    *errorOccurred=0;
    return fun;

error:
    /*
     * set the global error flag
     */
    *errorOccurred=1;
    send_error(conn, errmsg);
    free(errmsg);
    return NULL;
}

static char * create_r_func(callreq req) {
    int    plen;
    char * mrc;
    size_t mlen = 0;

    int i;

    // calculate space required for args
    for (i=0;i<req->nargs;i++){
        // +4 for , and space
        mlen += strlen(req->args[i].name) + 4;
    }
    /*
     * room for function source and function call
     */
    mlen += strlen(req->proc.src) + strlen(req->proc.name) + 40;

    mrc  = pmalloc(mlen);
    plen = snprintf(mrc,mlen,"%s <- function(",req->proc.name);


    for (i=0;i<req->nargs;i++){

        strcat( mrc,req->args[i].name);

        /* add a comma if not the last arg */
        if ( i < (req->nargs-1) ){
            strcat(mrc,", ") ;
            plen += 2;
        }

        /* keep track of where we are copying */
        plen+=strlen(req->args[i].name);
    }

    /* finish the function definition from where we left off */
    plen = snprintf(mrc+plen, mlen, ") {%s}", req->proc.src);
    assert(plen >= 0 && ((size_t)plen) < mlen);
    return mrc;
}





SEXP get_r_array(plcArray *plcArray)
{
	SEXP   vec;
	int *dims,ndim;

	int nr =1,
		nc=1,
		nz=1,
		i,j,k;

	ndim = plcArray->meta->ndims;
	dims = plcArray->meta->dims;

	if (ndim == 1)
	{
		nr = dims[0];
	}

	else if (ndim == 2)
	{
		nr = dims[0];
		nc = dims[1];
	}
	else if (ndim == 3)
	{
		nr = dims[0];
		nc = dims[1];
		nz = dims[2];
	}

	PROTECT(vec = get_r_vector(plcArray->meta->type, plcArray->meta->size));
	int isNull=0;
	int elem_idx=0;
	int elem_len =0;
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			for (k = 0; k < nz; k++)
			{
				//int	idx = (k * nr * nc) + (j * nr) + i;

				isNull = plcArray->nulls[elem_idx];
				elem_len = get_entry_length(plcArray->meta->type);

				if (!isNull){
					pg_get_one_r(plcArray->data+elem_idx * elem_len , plcArray->meta->type, &vec, elem_idx);
				}
				else{
					pg_get_null( plcArray->meta->type, &vec, elem_idx );
				}

				elem_idx++;
			}
		}
	}
	UNPROTECT(1);
	return vec;

}


SEXP convert_args(callreq req)
{
    SEXP    rargs, element;

    int    i;

    /* create the argument list */
    PROTECT(rargs = allocVector(VECSXP, req->nargs));

    for (i = 0; i < req->nargs; i++) {

        /*
        *  Use \N as null
        */
        if ( req->args[i].data.isnull == TRUE ) {
        	PROTECT(element=R_NilValue);
            SET_VECTOR_ELT( rargs, i, element );
            UNPROTECT(1);
        } else {
            switch( req->args[i].type.type ){

            case PLC_DATA_INT1:
                PROTECT(element = get_r_vector(PLC_DATA_INT1,1));
                LOGICAL(element)[0] = *((bool*)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                UNPROTECT(1);
                break;

            case PLC_DATA_TEXT:
                PROTECT(element = get_r_vector(PLC_DATA_TEXT,1));
                SET_STRING_ELT(element, 0, COPY_TO_USER_STRING(req->args[i].data.value));
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_INT2:
                PROTECT(element = get_r_vector(PLC_DATA_INT2,1));
                INTEGER(element)[0] = *((short *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_INT4:
                PROTECT(element = get_r_vector(PLC_DATA_INT4,1));
                INTEGER(element)[0] = *((int *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_INT8:
                PROTECT(element = get_r_vector(PLC_DATA_INT8,1));
                float tmp = *((int64 *)req->args[i].data.value);
                REAL(element)[0] = tmp ;
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_FLOAT4:
                PROTECT(element = get_r_vector(PLC_DATA_FLOAT4,1));
                REAL(element)[0] = *((float4 *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_FLOAT8:
                PROTECT(element = get_r_vector(PLC_DATA_FLOAT8,1));
                REAL(element)[0] = *((float8 *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_ARRAY:
            	PROTECT(element = get_r_array((plcArray *)req->args[i].data.value));
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_RECORD:
            case PLC_DATA_UDT:
            default:
                lprintf(ERROR, "unknown type %d", req->args[i].type.type);
            }
        }
    }
    UNPROTECT(1);
    return rargs;
}

static void
pg_get_null(plcDatatype column_type,  SEXP *obj, int elnum)
{
    switch (column_type)
    {
    	case PLC_DATA_INT2:
    	case PLC_DATA_INT4:
    		INTEGER_DATA(*obj)[elnum] = NA_INTEGER;
    		break;
        case PLC_DATA_INT8:
        case PLC_DATA_FLOAT4:
        case PLC_DATA_FLOAT8:
    		NUMERIC_DATA(*obj)[elnum] = NA_REAL;
    		break;
        case PLC_DATA_INT1:
            LOGICAL_DATA(*obj)[elnum] = NA_LOGICAL;
            break;
        case PLC_DATA_TEXT:
        	SET_STRING_ELT(*obj, elnum, NA_STRING);
        	break;
        case PLC_DATA_RECORD:
        case PLC_DATA_UDT:
        case PLC_DATA_ARRAY:
        default:
        	 lprintf(ERROR, "un-handled type %d",column_type);
    }
}

/*
 * given a single non-array pg value, convert to its R value representation
 */
static void
pg_get_one_r(char *value,  plcDatatype column_type, SEXP *obj, int elnum)
{
    switch (column_type)
    {
    /* 2 and 4 byte integer pgsql datatype => use R INTEGER */
        case PLC_DATA_INT2:
            INTEGER_DATA(*obj)[elnum] = *((int16 *)value);
            break;
        case PLC_DATA_INT4:
            INTEGER_DATA(*obj)[elnum] = *((int32 *)value);
            break;

            /*
             * Other numeric types => use R REAL
             * Note pgsql int8 is mapped to R REAL
             * because R INTEGER is only 4 byte
             */
        case PLC_DATA_INT8:
            NUMERIC_DATA(*obj)[elnum] = (int64)(*((float8 *)value));
            break;

        case PLC_DATA_FLOAT4:
            NUMERIC_DATA(*obj)[elnum] = *((float4 *)value);
            break;

        case PLC_DATA_FLOAT8:

            NUMERIC_DATA(*obj)[elnum] = *((float8 *)value);
            break;
        case PLC_DATA_INT1:
            LOGICAL_DATA(*obj)[elnum] = *((int8 *)value);
            break;
        case PLC_DATA_RECORD:
        case PLC_DATA_UDT:
        case PLC_DATA_INVALID:
        case PLC_DATA_ARRAY:
        	lprintf(ERROR, "unhandled type %d", column_type);
        	break;
        case PLC_DATA_TEXT:
        default:
            /* Everything else is defaulted to string */
            SET_STRING_ELT(*obj, elnum, COPY_TO_USER_STRING(*((char **)value)));
    }
}

/*
 * plr_SPI_exec - The builtin SPI_exec command for the R interpreter
 */
SEXP
plr_SPI_exec( SEXP rsql )
{
    const char             *sql;
    SEXP            r_result = NULL,
                    names,
                    row_names,
                    fldvec;

    int             res = 0,
                    i,j;

    char             buf[256];

    sql_msg_statement  msg;
    plcontainer_result result;
    message            resp;

    PROTECT(rsql =  AS_CHARACTER(rsql));
    sql = CHAR(STRING_ELT(rsql, 0));
    UNPROTECT(1);

    if (sql == NULL){
        error("%s", "cannot execute empty query");
        return NULL;
    }


    msg            = pmalloc(sizeof(*msg));
    msg->msgtype   = MT_SQL;
    msg->sqltype   = SQL_TYPE_STATEMENT;
    /*
     * satisfy compiler
     */
    msg->statement = (char *)sql;

    plcontainer_channel_send(plcconn, (message)msg);

    /* we don't need it anymore */
    pfree(msg);

    receive:
    res = plcontainer_channel_receive(plcconn, &resp);
    if (res < 0) {
        lprintf (ERROR, "Error receiving data from the backend, %d", res);
        return NULL;
    }

    switch (resp->msgtype) {
       case MT_CALLREQ:
          handle_call((callreq)resp, plcconn);
          free_callreq((callreq)resp);
          goto receive;
       case MT_RESULT:
           break;
       default:
           lprintf(WARNING, "didn't receive result back %c", resp->msgtype);
           return NULL;
    }

    result = (plcontainer_result)resp;
    if (result->rows == 0){
        return R_NilValue;
    }
    /*
     * r_result is a list of columns
     */
    PROTECT(r_result = NEW_LIST(result->cols));
    /*
     * names for each column
     */
    PROTECT(names = NEW_CHARACTER(result->cols));

    /*
     * we store everything in columns because vectors can only have one type
     * normally we get tuples back in rows with each column possibly a different type,
     * instead we store each column in a single vector
     */

    for (j=0; j<result->cols;j++){
        /*
         * set the names of the column
         */
        SET_STRING_ELT(names, j, Rf_mkChar(result->names[j]));

        //create a vector of the type that is rows long
        PROTECT(fldvec = get_r_vector(result->types[0].type, result->rows));

        for ( i=0; i<result->rows; i++ ){
            /*
             * store the value
             */
            pg_get_one_r(result->data[i][j].value, result->types[0].type, &fldvec, i);
        }

        UNPROTECT(1);
        SET_VECTOR_ELT(r_result, j, fldvec);
    }

    /* attach the column names */
    setAttrib(r_result, R_NamesSymbol, names);

    /* attach row names - basically just the row number, zero based */
    PROTECT(row_names = allocVector(STRSXP, result->rows));

    for ( i=0; i < result->rows; i++ ){
        sprintf(buf, "%d", i+1);
        SET_STRING_ELT(row_names, i, COPY_TO_USER_STRING(buf));
    }

    setAttrib(r_result, R_RowNamesSymbol, row_names);

    /* finally, tell R we are a data.frame */
    setAttrib(r_result, R_ClassSymbol, mkString("data.frame"));

    /*
     * result has
     *
     * an attribute names which is a vector of names
     * a vector of vectors num columns long by num rows
     */
    free_result(result);
    UNPROTECT(3);
    return r_result;
}

static SEXP
coerce_to_char(SEXP rval)
{
    SEXP    obj = NULL;

    switch (TYPEOF(rval))
    {
        case LISTSXP:
        case NILSXP:
        case SYMSXP:
        case VECSXP:
        case EXPRSXP:
        case LGLSXP:
        case INTSXP:
        case REALSXP:
        case CPLXSXP:
        case STRSXP:
        case RAWSXP:
            PROTECT(obj = AS_CHARACTER(rval));
            break;
        default:
            ;//TODO error
    }
    UNPROTECT(1);

    return obj;
}


void
throw_pg_notice(const char **msg)
{
    if (msg && *msg)
        last_R_notice = strdup(*msg);
}

void
throw_r_error(const char **msg)
{
    if (msg && *msg)
        last_R_error_msg = strdup(*msg);
    else
        last_R_error_msg = strdup("caught error calling R function");
}
