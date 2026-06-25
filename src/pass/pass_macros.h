#ifndef PASS_MACROS_H
#define PASS_MACROS_H

#define REGISTER_PASS(Manager, PassClass)                                      \
    (Manager).registerPass(#PassClass,                                         \
                           []() { return std::make_unique<PassClass>(); })

#define DECLARE_PASS(PassClass)                                                \
public:                                                                        \
    std::string getName() const override                                       \
    {                                                                          \
        return #PassClass;                                                     \
    }                                                                          \
                                                                               \
private:

#endif // PASS_MACROS_H