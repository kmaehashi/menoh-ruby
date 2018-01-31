#include <iostream>
#include <new>

#include "runx_ruby.hpp"

struct runx_ruby {
    onnx::ModelProto* onnx;
};

static runx_ruby* getONNX(VALUE self) {
    runx_ruby* p;
    Data_Get_Struct(self, runx_ruby, p);
    return p;
}

static void wrap_instant_free(runx_ruby* p) {
    p->onnx->~ModelProto();
    ruby_xfree(p);
}

static VALUE wrap_instant_alloc(VALUE klass) {
    void* p = ruby_xmalloc(sizeof(runx_ruby));
    return Data_Wrap_Struct(klass, NULL, wrap_instant_free, p);
}

static VALUE wrap_instant_init(VALUE self, VALUE vfilename) {
    char* filename = StringValuePtr(vfilename);
    // Load ONNX model
    getONNX(self)->onnx = new onnx::ModelProto(runx::load_onnx(filename));

    return Qnil;
}

struct runxModel {
    runx::model* model;
    int batch_size;
    int channel_num;
    int width;
    int height;
    std::string* input_layer;
    std::vector<std::string>* output_layers;
};

static runxModel* getModel(VALUE self) {
    runxModel* p;
    Data_Get_Struct(self, runxModel, p);
    return p;
}

static void wrap_model_free(runxModel* p) {
    delete p->model;
    delete p->input_layer;
    delete p->output_layers;
    ruby_xfree(p);
}

static VALUE wrap_model_alloc(VALUE klass) {
    void* p = ruby_xmalloc(sizeof(runxModel));
    return Data_Wrap_Struct(klass, NULL, wrap_model_free, p);
}

static VALUE wrap_model_init(VALUE self, VALUE vonnx, VALUE condition) {

    // TODO check data type

    // input data conditions
    int batch_size = getModel(self)->batch_size =
      NUM2INT(rb_hash_aref(condition, rb_to_symbol(rb_str_new2("batch_size"))));
    int channel_num = getModel(self)->channel_num = NUM2INT(
      rb_hash_aref(condition, rb_to_symbol(rb_str_new2("channel_num"))));
    int height = getModel(self)->height =
      NUM2INT(rb_hash_aref(condition, rb_to_symbol(rb_str_new2("height"))));
    int width = getModel(self)->width =
      NUM2INT(rb_hash_aref(condition, rb_to_symbol(rb_str_new2("width"))));

    // input_layer
    VALUE vinput_layer =
      rb_hash_aref(condition, rb_to_symbol(rb_str_new2("input_layer")));
    std::string* input_layer = new std::string(StringValuePtr(vinput_layer));
    getModel(self)->input_layer = input_layer;

    // output_layer
    std::vector<std::string>* output_layers = new std::vector<std::string>;
    VALUE voutput_layers =
      rb_hash_aref(condition, rb_to_symbol(rb_str_new2("output_layers")));
    int output_layer_num =
      NUM2INT(rb_funcall(voutput_layers, rb_intern("length"), 0, NULL));
    for(int i = 0; i < output_layer_num; i++) {
        VALUE voutput_layer = rb_ary_entry(voutput_layers, i);
        output_layers->push_back(std::string(StringValuePtr(voutput_layer)));
    }
    getModel(self)->output_layers = output_layers;

    std::vector<int> input_dims{batch_size, channel_num, height, width};

    runx::model* model = new runx::model(runx::make_model(
      *(getONNX(vonnx)->onnx),
      {std::make_tuple(*input_layer, runx::dtype_t::float_, input_dims,
                       mkldnn::memory::format::nchw)},
      *output_layers));
    getModel(self)->model = model;

    return Qnil;
}

static VALUE wrap_instant_makeModel(VALUE self, VALUE condition) {

    VALUE args[] = {self, condition};
    VALUE klass = rb_const_get(rb_cObject, rb_intern("RunxModel"));
    VALUE obj = rb_class_new_instance(2, args, klass);

    return obj;
}

static VALUE wrap_model_inference(VALUE self, VALUE images) {

    int image_num = NUM2INT(rb_funcall(images, rb_intern("length"), 0, NULL));

    // Copy input image data to model's input array
    auto& input_array =
      getModel(self)->model->input(*(getModel(self)->input_layer));

    // Convert RMagick format to instant format
    std::vector<float> image_data(
      getModel(self)->batch_size * getModel(self)->channel_num *
      getModel(self)->width * getModel(self)->height);
    for(int i; i < image_num; i++) {
        VALUE image = rb_ary_entry(images, i);
        VALUE raw_values =
          rb_funcall(image, rb_intern("export_pixels"), 0, NULL);
        auto value_num =
          NUM2INT(rb_funcall(raw_values, rb_intern("length"), 0, NULL));
        int image_offset = i * getModel(self)->channel_num *
                           getModel(self)->width * getModel(self)->height;
        for(int y = 0; y < getModel(self)->height; ++y) {
            for(int x = 0; x < getModel(self)->width; ++x) {
                for(int c = 0; c < getModel(self)->channel_num; c++) {
                    image_data[image_offset +
                               c * (getModel(self)->width *
                                    getModel(self)->height) +
                               y * getModel(self)->width + x] =
                      static_cast<float>(
                        NUM2INT(rb_ary_entry(
                          raw_values, getModel(self)->channel_num *
                                          (x + y * getModel(self)->height) +
                                        c)) /
                        257);
                }
            }
        }
    }
    std::copy(image_data.begin(), image_data.end(),
              runx::fbegin(input_array));

    // Run inference
    auto const& output_table = getModel(self)->model->run();

    // Get output
    VALUE results = rb_ary_new();
    for(int i = 0; i < getModel(self)->batch_size; i++) {
        VALUE result_each = rb_hash_new();
        for(auto output_layer : *(getModel(self)->output_layers)) {
            auto const& out_arr =
              runx::find_value(output_table, output_layer);
            int unit_num =
              runx::total_size(out_arr) / getModel(self)->batch_size;
            // Convert result to Ruby Array
            VALUE result_layer_output = rb_ary_new();
            for(int j = i * unit_num; j < (i + 1) * unit_num; ++j) {
                rb_ary_push(result_layer_output,
                            DBL2NUM(runx::fat(out_arr, j)));
            }

            rb_hash_aset(result_each, rb_str_new2(output_layer.c_str()),
                         result_layer_output);
        }
        rb_ary_push(results, result_each);
    }

    return results;
}

/**
 * will be called when required
 */

VALUE mRunx;

extern "C" void Init_runx_native() {

    mRunx = rb_define_module("Runx");

    VALUE onnx = rb_define_class_under(mRunx, "Runx", rb_cObject);

    rb_define_alloc_func(onnx, wrap_instant_alloc);
    rb_define_private_method(onnx, "initialize",
                             RUBY_METHOD_FUNC(wrap_instant_init), 1);
    rb_define_method(onnx, "make_model",
                     RUBY_METHOD_FUNC(wrap_instant_makeModel), 1);

    VALUE model = rb_define_class("RunxModel", rb_cObject);

    rb_define_alloc_func(model, wrap_model_alloc);
    rb_define_private_method(model, "initialize",
                             RUBY_METHOD_FUNC(wrap_model_init), 2);

    rb_define_method(model, "inference", RUBY_METHOD_FUNC(wrap_model_inference),
                     1);
}