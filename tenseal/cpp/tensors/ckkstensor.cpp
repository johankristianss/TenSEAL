#include "tenseal/cpp/tensors/ckkstensor.h"

namespace tenseal {

using namespace seal;
using namespace std;

CKKSTensor::CKKSTensor(const shared_ptr<TenSEALContext>& ctx,
                       const PlainTensor<double>& tensor,
                       std::optional<double> scale, bool batch)
    : _shape(tensor.shape()) {
    this->link_tenseal_context(ctx);
    if (scale.has_value()) {
        this->_init_scale = scale.value();
    } else {
        this->_init_scale = ctx->global_scale();
    }

    if (batch) {
        _batch_size = _shape[0];
        auto data = tensor.batch(0);

        for (auto it = data.cbegin(); it != data.cend(); it++)
            _data.push_back(CKKSTensor::encrypt(ctx, this->_init_scale, *it));

    } else {
        for (auto it = tensor.cbegin(); it != tensor.cend(); it++)
            _data.push_back(CKKSTensor::encrypt(ctx, this->_init_scale,
                                                vector<double>({*it})));
    }
}

CKKSTensor::CKKSTensor(const shared_ptr<TenSEALContext>& ctx,
                       const string& tensor) {
    this->link_tenseal_context(ctx);
    this->load(tensor);
}

CKKSTensor::CKKSTensor(const TenSEALContextProto& ctx,
                       const CKKSTensorProto& tensor) {
    this->load_context_proto(ctx);
    this->load_proto(tensor);
}

CKKSTensor::CKKSTensor(const shared_ptr<TenSEALContext>& ctx,
                       const CKKSTensorProto& tensor) {
    this->link_tenseal_context(ctx);
    this->load_proto(tensor);
}

CKKSTensor::CKKSTensor(const shared_ptr<const CKKSTensor>& tensor) {
    this->link_tenseal_context(tensor->tenseal_context());
    this->_init_scale = tensor->scale();
    this->_shape = tensor->shape();
    this->_data = tensor->data();
    this->_batch_size = tensor->_batch_size;
}

Ciphertext CKKSTensor::encrypt(const shared_ptr<TenSEALContext>& ctx,
                               const double scale, const vector<double>& data) {
    if (data.empty()) {
        throw invalid_argument("Attempting to encrypt an empty vector");
    }
    auto slot_count = ctx->slot_count<CKKSEncoder>();
    if (data.size() > slot_count)
        // number of slots available is poly_modulus_degree / 2
        throw invalid_argument(
            "can't encrypt vectors of this size, please use a larger "
            "polynomial modulus degree.");

    Ciphertext ciphertext(*ctx->seal_context());
    Plaintext plaintext;
    ctx->encode<CKKSEncoder>(data, plaintext, scale);
    ctx->encryptor->encrypt(plaintext, ciphertext);

    return ciphertext;
}

PlainTensor<double> CKKSTensor::decrypt(const shared_ptr<SecretKey>& sk) const {
    Plaintext plaintext;
    Decryptor decryptor =
        Decryptor(*this->tenseal_context()->seal_context(), *sk);

    if (_batch_size) {
        vector<vector<double>> result;
        result.reserve(this->_data.size());

        for (size_t i = 0; i < this->_data.size(); i++) {
            vector<double> buff;
            decryptor.decrypt(this->_data[i], plaintext);
            this->tenseal_context()->decode<CKKSEncoder>(plaintext, buff);
            result.push_back(
                vector<double>(buff.begin(), buff.begin() + *_batch_size));
        }

        return PlainTensor<double>(/*batched_tensor=*/result,
                                   /*original_shape=*/_shape, /*batch_axis=*/0);
    } else {
        vector<double> result;
        result.reserve(this->_data.size());

        for (size_t i = 0; i < this->_data.size(); i++) {
            vector<double> buff;
            decryptor.decrypt(this->_data[i], plaintext);
            this->tenseal_context()->decode<CKKSEncoder>(plaintext, buff);
            result.push_back(buff[0]);
        }

        return PlainTensor<double>(result, /*original_shape=*/_shape);
    }
}

shared_ptr<CKKSTensor> CKKSTensor::negate_inplace() {
    for (auto& ct : _data)
        this->tenseal_context()->evaluator->negate_inplace(ct);
    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::square_inplace() {
    for (auto& ct : _data)
        this->tenseal_context()->evaluator->square_inplace(ct);
    this->auto_relin(_data);
    this->auto_rescale(_data);
    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::power_inplace(unsigned int power) {
    if (power == 0) {
        auto ones = PlainTensor<double>::repeat_value(1, this->shape());
        *this = CKKSTensor(this->tenseal_context(), ones, this->_init_scale,
                           _batch_size.has_value());
        return shared_from_this();
    }

    if (power == 1) {
        return shared_from_this();
    }

    if (power == 2) {
        this->square_inplace();
        return shared_from_this();
    }

    int closest_power_of_2 = 1 << static_cast<int>(floor(log2(power)));
    power -= closest_power_of_2;
    if (power == 0) {
        this->power_inplace(closest_power_of_2 / 2)->square_inplace();
    } else {
        auto closest_pow2_vector = this->power(closest_power_of_2);
        this->power_inplace(power)->mul_inplace(closest_pow2_vector);
    }

    return shared_from_this();
}

void CKKSTensor::perform_op(seal::Ciphertext& ct, seal::Ciphertext other,
                            OP op) {
    this->auto_same_mod(other, ct);
    switch (op) {
        case OP::ADD:
            this->tenseal_context()->evaluator->add_inplace(ct, other);
            break;
        case OP::SUB:
            this->tenseal_context()->evaluator->sub_inplace(ct, other);
            break;
        case OP::MUL:
            this->tenseal_context()->evaluator->multiply_inplace(ct, other);
            this->auto_relin(ct);
            this->auto_rescale(ct);
            break;
        default:
            throw invalid_argument("operation not defined");
    }
}

void CKKSTensor::perform_plain_op(seal::Ciphertext& ct, seal::Plaintext other,
                                  OP op) {
    this->auto_same_mod(other, ct);
    switch (op) {
        case OP::ADD:
            this->tenseal_context()->evaluator->add_plain_inplace(ct, other);
            break;
        case OP::SUB:
            this->tenseal_context()->evaluator->sub_plain_inplace(ct, other);
            break;
        case OP::MUL:
            try {
                this->tenseal_context()->evaluator->multiply_plain_inplace(
                    ct, other);
            } catch (const std::logic_error& e) {
                if (strcmp(e.what(), "result ciphertext is transparent") == 0) {
                    // replace by encryption of zero
                    this->tenseal_context()->encryptor->encrypt_zero(ct);
                    ct.scale() = this->_init_scale;
                } else {  // Something else, need to be forwarded
                    throw;
                }
            }
            this->auto_relin(ct);
            this->auto_rescale(ct);
            break;
        default:
            throw invalid_argument("operation not defined");
    }
}

shared_ptr<CKKSTensor> CKKSTensor::op_inplace(
    const shared_ptr<CKKSTensor>& operand, OP op) {
    // TODO implement broadcasting
    if (this->_shape != operand->_shape) {
        // TODO provide a better message (what are the shapes)
        throw invalid_argument("Operands don't have the same shape");
    }

    size_t n_jobs = this->tenseal_context()->dispatcher_size();

    auto worker_func = [&](size_t start, size_t end) -> bool {
        for (size_t i = start; i < end; i++) {
            this->perform_op(this->_data[i], operand->_data[i], op);
        }
        return true;
    };

    if (n_jobs == 1) {
        worker_func(0, this->_data.size());
    } else {
        size_t batch_size = (this->_data.size() + n_jobs - 1) / n_jobs;
        vector<future<bool>> futures;
        for (size_t i = 0; i < n_jobs; i++) {
            futures.push_back(
                this->tenseal_context()->dispatcher()->enqueue_task(
                    worker_func, i * batch_size,
                    std::min((i + 1) * batch_size, this->_data.size())));
        }
        // waiting
        optional<string> fail;
        for (size_t i = 0; i < futures.size(); i++) {
            try {
                futures[i].get();
            } catch (std::exception& e) {
                fail = e.what();
            }
        }

        if (fail) throw invalid_argument(fail.value());
    }

    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::op_plain_inplace(
    const PlainTensor<double>& operand, OP op) {
    // TODO implement broadcasting
    // TODO batched ops
    if (this->_shape != operand.shape()) {
        // TODO provide a better message (what are the shapes)
        throw invalid_argument("Operands don't have the same shape");
    }

    size_t n_jobs = this->tenseal_context()->dispatcher_size();
    auto operand_data = operand.data();

    auto worker_func = [&](size_t start, size_t end) -> bool {
        Plaintext plaintext;
        for (size_t i = start; i < end; i++) {
            this->tenseal_context()->encode<CKKSEncoder>(
                operand_data[i], plaintext, this->_init_scale);
            this->perform_plain_op(this->_data[i], plaintext, op);
        }
        return true;
    };

    if (n_jobs == 1) {
        worker_func(0, this->_data.size());
    } else {
        size_t batch_size = (this->_data.size() + n_jobs - 1) / n_jobs;
        vector<future<bool>> futures;
        for (size_t i = 0; i < n_jobs; i++) {
            futures.push_back(
                this->tenseal_context()->dispatcher()->enqueue_task(
                    worker_func, i * batch_size,
                    std::min((i + 1) * batch_size, this->_data.size())));
        }
        // waiting
        optional<string> fail;
        for (size_t i = 0; i < futures.size(); i++) {
            try {
                futures[i].get();
            } catch (std::exception& e) {
                fail = e.what();
            }
        }

        if (fail) {
            throw invalid_argument(fail.value());
        }
    }

    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::op_plain_inplace(const double& operand,
                                                    OP op) {
    size_t n_jobs = this->tenseal_context()->dispatcher_size();
    Plaintext plaintext;
    this->tenseal_context()->encode<CKKSEncoder>(operand, plaintext,
                                                 this->_init_scale);

    auto worker_func = [&](size_t start, size_t end) -> bool {
        for (size_t i = start; i < end; i++) {
            this->perform_plain_op(this->_data[i], plaintext, op);
        }
        return true;
    };

    if (n_jobs == 1) {
        worker_func(0, this->_data.size());
    } else {
        size_t batch_size = (this->_data.size() + n_jobs - 1) / n_jobs;
        vector<future<bool>> futures;
        for (size_t i = 0; i < n_jobs; i++) {
            futures.push_back(
                this->tenseal_context()->dispatcher()->enqueue_task(
                    worker_func, i * batch_size,
                    std::min((i + 1) * batch_size, this->_data.size())));
        }
        // waiting
        std::optional<std::string> fail;
        for (size_t i = 0; i < futures.size(); i++) {
            try {
                futures[i].get();
            } catch (std::exception& e) {
                fail = e.what();
            }
        }

        if (fail) throw invalid_argument(fail.value());
    }

    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::add_inplace(
    const shared_ptr<CKKSTensor>& to_add) {
    return this->op_inplace(to_add, OP::ADD);
}

shared_ptr<CKKSTensor> CKKSTensor::sub_inplace(
    const shared_ptr<CKKSTensor>& to_sub) {
    return this->op_inplace(to_sub, OP::SUB);
}

shared_ptr<CKKSTensor> CKKSTensor::mul_inplace(
    const shared_ptr<CKKSTensor>& to_mul) {
    return this->op_inplace(to_mul, OP::MUL);
}

shared_ptr<CKKSTensor> CKKSTensor::dot_product_inplace(
    const shared_ptr<CKKSTensor>& to_mul) {
    // TODO
    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::add_plain_inplace(
    const PlainTensor<double>& to_add) {
    return this->op_plain_inplace(to_add, OP::ADD);
}

shared_ptr<CKKSTensor> CKKSTensor::sub_plain_inplace(
    const PlainTensor<double>& to_sub) {
    return this->op_plain_inplace(to_sub, OP::SUB);
}

shared_ptr<CKKSTensor> CKKSTensor::mul_plain_inplace(
    const PlainTensor<double>& to_mul) {
    return this->op_plain_inplace(to_mul, OP::MUL);
}

shared_ptr<CKKSTensor> CKKSTensor::dot_product_plain_inplace(
    const PlainTensor<double>& to_mul) {
    // TODO
    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::add_plain_inplace(const double& to_add) {
    return this->op_plain_inplace(to_add, OP::ADD);
}

shared_ptr<CKKSTensor> CKKSTensor::sub_plain_inplace(const double& to_sub) {
    return this->op_plain_inplace(to_sub, OP::SUB);
}

shared_ptr<CKKSTensor> CKKSTensor::mul_plain_inplace(const double& to_mul) {
    return this->op_plain_inplace(to_mul, OP::MUL);
}

shared_ptr<CKKSTensor> CKKSTensor::sum_inplace(size_t axis) {
    if (axis >= _shape.size()) throw invalid_argument("invalid axis");

    if (_batch_size && axis == 0) return sum_batch_inplace();

    auto new_shape = _shape;
    auto new_len = _data.size() / _shape[axis];

    // remove the summation axis
    new_shape.erase(new_shape.begin() + axis);

    auto working_shape = _shape;
    if (_batch_size) {
        // remove the batched axis
        working_shape.erase(working_shape.begin());
        new_shape.erase(new_shape.begin());
        axis--;
    }

    std::vector<Ciphertext> new_data(new_len);
    vector<vector<Ciphertext>> batches(new_len);

    auto old_strides = generate_strides(working_shape);
    auto new_strides = generate_strides(new_shape);

    for (size_t idx = 0; idx < _data.size(); ++idx) {
        auto pos = position_from_strides(old_strides, idx);
        pos.erase(pos.begin() + axis);

        size_t new_idx = 0;
        for (size_t pidx = 0; pidx < pos.size(); ++pidx)
            new_idx += new_strides[pidx] * pos[pidx];

        batches[new_idx].push_back(_data[idx]);
    }

    for (size_t idx = 0; idx < new_len; ++idx) {
        tenseal_context()->evaluator->add_many(batches[idx], new_data[idx]);
    }

    if (_batch_size) {
        // reinsert the batched axis
        new_shape.insert(new_shape.begin(), *_batch_size);
    }

    _data = new_data;
    _shape = new_shape;
    return shared_from_this();
}
shared_ptr<CKKSTensor> CKKSTensor::sum_batch_inplace() {
    if (!_batch_size) throw invalid_argument("unsupported operation");

    for (size_t idx = 0; idx < _data.size(); ++idx) {
        sum_vector(this->tenseal_context(), _data[idx], *_batch_size);
    }

    _batch_size = {};
    _shape.erase(_shape.begin());
    return shared_from_this();
}

shared_ptr<CKKSTensor> CKKSTensor::polyval_inplace(
    const vector<double>& coefficients) {
    if (coefficients.size() == 0) {
        throw invalid_argument(
            "the coefficients vector need to have at least one element");
    }

    int degree = static_cast<int>(coefficients.size()) - 1;
    while (degree >= 0) {
        if (coefficients[degree] == 0.0)
            degree--;
        else
            break;
    }

    if (degree == -1) {
        auto zeros = PlainTensor<double>::repeat_value(0, this->shape());
        *this = CKKSTensor(this->tenseal_context(), zeros, this->_init_scale,
                           _batch_size.has_value());
        return shared_from_this();
    }

    // pre-compute squares of x
    auto x = this->copy();

    int max_square = static_cast<int>(floor(log2(degree)));
    vector<shared_ptr<CKKSTensor>> x_squares;
    x_squares.reserve(max_square + 1);
    x_squares.push_back(x->copy());  // x
    for (int i = 1; i <= max_square; i++) {
        x->square_inplace();
        x_squares.push_back(x->copy());  // x^(2^i)
    }

    auto cst_coeff =
        PlainTensor<double>::repeat_value(coefficients[0], this->shape());
    auto result =
        CKKSTensor::Create(this->tenseal_context(), cst_coeff,
                           this->_init_scale, _batch_size.has_value());

    // coefficients[1] * x + ... + coefficients[degree] * x^(degree)
    for (int i = 1; i <= degree; i++) {
        if (coefficients[i] == 0.0) continue;
        x = compute_polynomial_term(i, coefficients[i], x_squares);
        result->add_inplace(x);
    }

    this->_data = result->data();
    return shared_from_this();
}

void CKKSTensor::clear() {
    this->_shape = vector<size_t>();
    this->_data = vector<Ciphertext>();
    this->_batch_size = optional<double>();
    this->_init_scale = 0;
}

void CKKSTensor::load_proto(const CKKSTensorProto& tensor_proto) {
    if (this->tenseal_context() == nullptr) {
        throw invalid_argument("context missing for deserialization");
    }
    this->clear();

    for (int idx = 0; idx < tensor_proto.shape_size(); ++idx) {
        this->_shape.push_back(tensor_proto.shape(idx));
    }
    for (int idx = 0; idx < tensor_proto.ciphertexts_size(); ++idx)
        this->_data.push_back(SEALDeserialize<Ciphertext>(
            *this->tenseal_context()->seal_context(),
            tensor_proto.ciphertexts(idx)));
    this->_init_scale = tensor_proto.scale();
    if (tensor_proto.batch_size())
        this->_batch_size = tensor_proto.batch_size();
}

CKKSTensorProto CKKSTensor::save_proto() const {
    CKKSTensorProto buffer;

    for (auto& ct : this->_data) {
        buffer.add_ciphertexts(SEALSerialize<Ciphertext>(ct));
    }
    for (auto& dim : this->_shape) {
        buffer.add_shape(dim);
    }
    buffer.set_scale(this->_init_scale);
    if (this->_batch_size) buffer.set_batch_size(*this->_batch_size);

    return buffer;
}

void CKKSTensor::load(const std::string& tensor_str) {
    CKKSTensorProto buffer;
    if (!buffer.ParseFromArray(tensor_str.c_str(),
                               static_cast<int>(tensor_str.size()))) {
        throw invalid_argument("failed to parse CKKS tensor stream");
    }
    this->load_proto(buffer);
}

std::string CKKSTensor::save() const {
    auto buffer = this->save_proto();
    std::string output;
    output.resize(proto_bytes_size(buffer));

    if (!buffer.SerializeToArray((void*)output.c_str(),
                                 static_cast<int>(proto_bytes_size(buffer)))) {
        throw invalid_argument("failed to save CKKS tensor proto");
    }

    return output;
}

shared_ptr<CKKSTensor> CKKSTensor::copy() const {
    return shared_ptr<CKKSTensor>(new CKKSTensor(shared_from_this()));
}

shared_ptr<CKKSTensor> CKKSTensor::deepcopy() const {
    TenSEALContextProto ctx = this->tenseal_context()->save_proto();
    CKKSTensorProto vec = this->save_proto();
    return CKKSTensor::Create(ctx, vec);
}

vector<Ciphertext> CKKSTensor::data() const { return _data; }
vector<size_t> CKKSTensor::shape() const { return _shape; }
double CKKSTensor::scale() const { return _init_scale; }
}  // namespace tenseal