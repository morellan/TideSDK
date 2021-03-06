/**
* This file has been modified from its orginal sources.
*
* Copyright (c) 2012 Software in the Public Interest Inc (SPI)
* Copyright (c) 2012 David Pratt
* 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
***
* Copyright (c) 2008-2012 Appcelerator Inc.
* 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#include "ruby_module.h"
#include <cstring>
#include <iostream>
#include <sstream>

namespace tide
{
    RubyEvaluator::RubyEvaluator() :
        StaticBoundObject("Ruby.Evaluator")
    {
        /**
         * @notiapi(method=True,name=Ruby.canEvaluate,since=0.7)
         * @notiarg[String, mimeType] Code mime type
         * @notiresult[bool] whether or not the mimetype is understood by Ruby
         */
        SetMethod("canEvaluate", &RubyEvaluator::CanEvaluate);
        
        /**
         * @notiapi(method=Ruby,name=Ruby.evaluate,since=0.2) Evaluates a string as Ruby code
         * @notiarg[String, mimeType] Code mime type (normally "text/ruby")
         * @notiarg[String, name] name of the script source
         * @notiarg[String, code] Ruby script code
         * @notiarg[Object, scope] global variable scope
         * @notiresult[Any] result of the evaluation
         */
        SetMethod("evaluate", &RubyEvaluator::Evaluate);
    }

    RubyEvaluator::~RubyEvaluator()
    {
    }

    static TiObjectRef global_object;
    static VALUE m_missing(int argc, VALUE* argv, VALUE self)
    {
        bool assignment = false;

        if (global_object.isNull())
            return Qnil;

        // store the method name and arguments in separate variables
        VALUE method_name, args;
        rb_scan_args(argc, argv, "1*", &method_name, &args);
        char* name = strdup(rb_id2name(SYM2ID(method_name)));

        // Check if this is an assignment
        if (name[strlen(name) - 1] == '=')
        {
            name[strlen(name) - 1] = '\0';
            assignment = true;
        }
        // If we can't find this property perhaps we should return
        // the same property name except capitalized.
        ValueRef v = global_object->Get(name);
        if (v->IsUndefined())
        {
            name[0] = toupper(name[0]);
            v = global_object->Get(name);
        }
        // Okay, maybe not
        if (v->IsUndefined())
            name[0] = tolower(name[0]);

        VALUE rval;
        if (assignment) // Assignment
        {
            rval = rb_ary_entry(args, 0);
            ValueRef val = RubyUtils::ToTiValue(rval);
            global_object->Set(name, val);
        }
        else if (v->IsMethod()) // Method call
        {
            rval = RubyUtils::GenericTiMethodCall(v->ToMethod(), args);
        }
        else // Plain old access
        {
            rval = RubyUtils::ToRubyValue(v);
        }
        return rval;
    }

    std::string RubyEvaluator::GetContextId(TiObjectRef global)
    {
        static int nextId = 0;
        int cid = 0;
        ValueRef idv = global->Get("__ruby_module_id__");
        if (idv->IsUndefined())
        {
            cid = nextId++;
            global->Set("__ruby_module_id__", Value::NewInt(cid));
        }
        else
        {
            cid = idv->ToInt();
        }
        return std::string("$windowProc") + TiList::IntToChars(cid);
    }

    VALUE RubyEvaluator::GetContext(TiObjectRef global)
    {
        std::string theid = this->GetContextId(global);
        VALUE ctx = rb_gv_get(theid.c_str());
        if (ctx == Qnil)
        {
            VALUE ctx_class = rb_define_class("TideRubyContext", rb_cObject);
            rb_define_method(ctx_class, "method_missing", VALUEFUNC(m_missing), -1); 
            ctx = rb_obj_alloc(ctx_class);
            rb_gv_set(theid.c_str(), ctx);
        }
        return ctx;
    }

    VALUE reval_do_call(VALUE args)
    {
        // Don't use rb_obj_instance_eval here, as it will implicitly
        // use any Ruby code block that was passed. See:
        // http://banisterfiend.wordpress.com/2008/09/25/metaprogramming-in-the-ruby-c-api-part-one-blocks/
        VALUE ctx = rb_ary_shift(args);
        VALUE code = rb_ary_shift(args);
        return rb_funcall(ctx, rb_intern("instance_eval"), 1, code);
    }
    
    void RubyEvaluator::CanEvaluate(const ValueList& args, ValueRef result)
    {
        args.VerifyException("canEvaluate", "s");
        std::string mimeType = args.GetString(0);
        result->SetBool(mimeType == "text/ruby");
    }

    void RubyEvaluator::Evaluate(const ValueList& args, ValueRef result)
    {
        args.VerifyException("evaluate", "s s s o");
        
        //const char *mimeType = args.GetString(0).c_str();
        std::string name = args.GetString(1);
        std::string code = args.GetString(2);
        global_object = args.GetObject(3);

        VALUE ctx = this->GetContext(global_object);

        VALUE rargs = rb_ary_new();
        rb_ary_push(rargs, ctx);
        rb_ary_push(rargs, rb_str_new2(code.c_str()));

        int error;
        VALUE returnValue = rb_protect(reval_do_call, rargs, &error);
        RubyEvaluator::ContextToGlobal(ctx, global_object);

        if (error != 0)
        {
            std::string error("An error occured while parsing Ruby (");
            error += name;
            error += "): ";

            // Display a stringified version of the exception.
            VALUE exception = rb_gv_get("$!");
            ValueRef v = RubyUtils::ToTiValue(exception);
            SharedString ss = v->DisplayString();
            error.append(ss->c_str());

            // Try to make a nice backtrace for the user.
            VALUE backtrace = rb_funcall(exception,
                rb_intern("backtrace"), 0);
            VALUE rBacktraceString = rb_funcall(backtrace,
                rb_intern("join"), 1, rb_str_new2("\n"));
            if (TYPE(rBacktraceString) == T_STRING)
            {
                error.append("\n");
                error.append(StringValuePtr(rBacktraceString));
            }

            Logger *logger = Logger::Get("Ruby");
            logger->Error(error);

            result->SetUndefined();
            return;
        }

        result->SetValue(RubyUtils::ToTiValue(returnValue));
    }

    void RubyEvaluator::ContextToGlobal(VALUE ctx, TiObjectRef o)
    {
        if (global_object.isNull())
            return;

        // Next copy all methods over -- they override variables
        VALUE methods = rb_funcall(ctx, rb_intern("singleton_methods"), 0);
        for (long i = 0; i < RARRAY_LEN(methods); i++)
        {
            VALUE meth_symbol = rb_ary_entry(methods, i);
            const char* meth_name = STR2CSTR(meth_symbol);

            // Skip our special method_missing method
            if (strcmp(meth_name, "method_missing") == 0)
                continue;

            volatile VALUE rmeth = rb_funcall(ctx, rb_intern("method"), 1, meth_symbol);
            TiMethodRef method = new KRubyMethod(rmeth, meth_name);
            o->SetMethod(meth_name, method);
        }
    }
}
