// Compiler attempts are runtime transactions. A free kernel lock is not proof
// that an earlier writer is gone; the runtime reservation names its unit.
export module mcpp.build.schedule.compiler_transaction;
import std;
import mcpplibs.os.fs;
import mcpplibs.os.process;
import mcpplibs.os.ipc;
import mcpplibs.os.dirs;
import mcpp.home;
import mcpp.build.stage;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::schedule::transaction {
namespace fs = std::filesystem;
namespace os = mcpplibs::os;
using Fields = std::vector<std::string>;
using Record = std::expected<std::optional<Fields>,std::string>;
using ObserverMap = std::map<std::string,os::process::unit_observer>;

template<class T> bool decimal(std::string_view text) {
    T value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    return !text.empty()&&parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size();
}
bool digest(std::string_view text) {
    return text.size()==16&&std::ranges::all_of(text,[](unsigned char c) {
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
bool output_stamp(std::string_view size,std::string_view time) {
    return (size=="missing"&&time=="missing")
        ||(decimal<std::uintmax_t>(size)&&decimal<long long>(time));
}

std::string pack(std::string_view tag, const Fields& fields) {
    std::string body = std::string(tag) + "\n" + std::to_string(fields.size()) + "\n";
    for (const auto& field : fields) body += std::to_string(field.size()) + "\n" + field;
    return body + "\n" + mcpp::toolchain::hash_string(body) + "\n";
}
// Atomic visibility across process loss; storage durability is not promised.
bool replace_record(const fs::path& path,std::string_view bytes);
bool publish(const fs::path& path,std::string_view tag,const Fields& fields);
Record read(const fs::path& path, std::string_view tag) {
    std::ifstream input(path,std::ios::binary);
    if (!input) {
        std::error_code ec;
        if (!fs::exists(path,ec) && !ec) return std::optional<Fields>{};
        if(ec)return std::unexpected("cannot read compiler transaction record");
        // An atomic publisher may install the record between the failed open
        // and the existence probe. Retry once; a persistent refusal still
        // fails rather than being mistaken for an absent record.
        input.open(path,std::ios::binary);
        if(!input)return std::unexpected("cannot read compiler transaction record");
    }
    std::string bytes(65537,'\0');
    input.read(bytes.data(),bytes.size());bytes.resize(static_cast<std::size_t>(input.gcount()));
    if(input.bad())return std::unexpected("cannot read compiler transaction record");
    if (bytes.size() < 18 || bytes.size() > 65536 || bytes[bytes.size()-18] != '\n' || bytes.back() != '\n')
        return std::unexpected("corrupt compiler transaction record");
    const auto body = std::string_view(bytes).substr(0,bytes.size()-18);
    if (mcpp::toolchain::hash_string(body) != std::string_view(bytes).substr(bytes.size()-17,16))
        return std::unexpected("compiler transaction checksum mismatch");
    std::size_t at=0;
    auto word = [&]() -> std::optional<std::string_view> {
        const auto end=body.find('\n',at); if (end==body.npos) return {};
        const auto value=body.substr(at,end-at);at=end+1;return value;
    };
    auto number = [&]() -> std::optional<std::size_t> {
        const auto text=word();if(!text)return {};
        std::size_t value=0;const auto result=std::from_chars(text->data(),text->data()+text->size(),value);
        if(result.ec!=std::errc{}||result.ptr!=text->data()+text->size())return {};return value;
    };
    const auto schema=word();const auto count=number();
    if(!schema||*schema!=tag||!count||*count>32)return std::unexpected("unknown compiler transaction schema");
    Fields fields;
    for(std::size_t i=0;i<*count;++i) {
        const auto n=number();if(!n||*n>body.size()-at)return std::unexpected("truncated compiler transaction record");
        fields.emplace_back(body.substr(at,*n));at+=*n;
    }
    if(at!=body.size())return std::unexpected("unexpected compiler transaction record data");
    return std::optional<Fields>{std::move(fields)};
}
std::expected<fs::path,std::string> output_identity(const fs::path& path) {
    if(path.empty())return fs::path{};
    std::error_code ec;const auto absolute=fs::absolute(path,ec).lexically_normal();
    if(ec)return std::unexpected("cannot resolve a compiler output identity");
    const auto parent=fs::weakly_canonical(absolute.parent_path(),ec);
    if(ec)return std::unexpected("cannot resolve a compiler output directory");
    return parent/absolute.filename();
}
std::string spelling(const fs::path& path) { return mcpp::home::spelling(path); }
fs::path named(std::string_view path) { return mcpp::home::from_utf8(path); }
fs::path suffix(const fs::path& path,std::string_view tail) { return named(spelling(path)+std::string(tail)); }
std::optional<long long> journal_time(fs::file_time_type time) {
    // libc++ uses a wider file-clock representation. The v1 journal accepts
    // signed 64-bit ticks; never truncate a timestamp into a different identity.
    using Rep=fs::file_time_type::duration::rep;
    const Rep ticks=time.time_since_epoch().count();
    if constexpr(std::numeric_limits<Rep>::digits>std::numeric_limits<long long>::digits) {
        if(ticks>static_cast<Rep>(std::numeric_limits<long long>::max()))return {};
        if constexpr(std::numeric_limits<Rep>::is_signed)
            if(ticks<static_cast<Rep>(std::numeric_limits<long long>::min()))return {};
    }
    return static_cast<long long>(ticks);
}
std::optional<Fields> identity(const fs::path& path) {
    if(path.empty())return Fields{"", ""};
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(ec||status.type()!=fs::file_type::regular)return {};
    const auto size=fs::file_size(path,ec);if(ec)return {};
    const auto time=fs::last_write_time(path,ec);if(ec)return {};
    const auto ticks=journal_time(time);if(!ticks)return {};
    return Fields{std::to_string(size),std::to_string(*ticks)};
}
bool copy_atomic(const fs::path& from,const fs::path& to) {
    std::error_code ec;const auto temp=suffix(to,".mcpp-restore");
    fs::copy_file(from,temp,fs::copy_options::overwrite_existing,ec);if(ec)return false;
    const auto time=fs::last_write_time(from,ec);if(ec)return false;
    fs::last_write_time(temp,time,ec);if(ec)return false;
    return bool(os::fs::rename(temp,to,true));
}
struct Attempt {
    fs::path directory, bmi, object, slot, dep;
    bool ordinary=false, before_present=false;
    std::string before_size,before_time,before_hash;
    static std::expected<Attempt,std::string> load(const fs::path& directory) {
        auto record=read(directory/"prepared","mcpp-attempt-v1");
        if(!record)return std::unexpected(record.error());
        if(!*record||(**record).size()!=9)return std::unexpected("missing or invalid prepared compiler transaction");
        const auto& v=**record;
        if((v[0]!="ordinary"&&v[0]!="bmi")||(v[5]!="0"&&v[5]!="1"))return std::unexpected("invalid compiler transaction fields");
        Attempt a; a.directory=directory;a.ordinary=v[0]=="ordinary";a.bmi=named(v[1]);a.object=named(v[2]);a.slot=named(v[3]);a.dep=named(v[4]);
        a.before_present=v[5]=="1";a.before_size=v[6];a.before_time=v[7];a.before_hash=v[8];
        if(a.object.empty()||(!a.ordinary&&(a.slot.empty()||a.bmi.empty()))
            ||(a.before_present&&(!decimal<std::uintmax_t>(v[6])||!decimal<long long>(v[7])||!digest(v[8])))
            ||(!a.before_present&&(!v[6].empty()||!v[7].empty()||!v[8].empty())))
            return std::unexpected("invalid prepared compiler output identity");
        for(const auto& p:{a.bmi,a.object,a.slot,a.dep})if(!p.empty()&&!p.is_absolute())return std::unexpected("relative compiler output in transaction");
        return a;
    }
    static std::expected<Attempt,std::string> prepare(const fs::path& generation,bool ordinary,
        const fs::path& bmi,const fs::path& object,const fs::path& slot,const fs::path& dep) {
        Attempt a;a.ordinary=ordinary;
        auto b=output_identity(bmi),o=output_identity(object),s=output_identity(slot),d=output_identity(dep);
        if(!b||!o||!s||!d)return std::unexpected("cannot identify compiler transaction outputs");
        a.bmi=*b;a.object=*o;a.slot=*s;a.dep=*d;
        if(a.object.empty()||(!ordinary&&(a.bmi.empty()||a.slot.empty())))
            return std::unexpected("missing compiler transaction output identity");
        if(!a.bmi.empty())if(auto old=identity(a.bmi)) {
            a.before_present=true;a.before_size=(*old)[0];a.before_time=(*old)[1];a.before_hash=mcpp::toolchain::hash_file(a.bmi);
        } else {
            std::error_code ec;
            if(fs::exists(a.bmi,ec)||ec)return std::unexpected("cannot identify the previous compiler BMI");
        }
        const auto who=os::process::self();if(!who||!who->id||!who->started_ns)return std::unexpected("cannot identify compiler transaction writer");
        static std::atomic<unsigned long long> sequence{0};
        a.directory=generation/"attempts"/std::format("{}-{}-{}",who->id,who->started_ns,sequence++);
        std::error_code ec;fs::create_directories(a.directory,ec);if(ec)return std::unexpected("cannot create compiler transaction");
        if(!publish(a.directory/"prepared","mcpp-attempt-v1",{ordinary?"ordinary":"bmi",spelling(a.bmi),spelling(a.object),spelling(a.slot),spelling(a.dep),a.before_present?"1":"0",a.before_size,a.before_time,a.before_hash}))
            return std::unexpected("cannot publish compiler transaction before output modification");
        if(!a.slot.empty()&&!publish(suffix(a.slot,".transaction"),"mcpp-attempt-pointer-v1",{spelling(a.directory)}))
            return std::unexpected("cannot publish compiler transaction pointer");
        return a;
    }
    fs::path backup() const { return directory/"baseline.bmi"; }
    std::expected<void,std::string> prepare_backup() const {
        if(bmi.empty())return {};
        fs::path source;std::string hash;
        if(before_present) {source=bmi;hash=before_hash;}
        else {
            auto carried=read(suffix(bmi,".bak.cert"),"mcpp-carried-baseline-v1");
            if(!carried)return std::unexpected(carried.error());
            if(*carried) {
                const auto& v=**carried;
                if(v.size()!=2||v[0]!=spelling(bmi)||!digest(v[1])||!identity(suffix(bmi,".bak"))
                    ||v[1]!=mcpp::toolchain::hash_file(suffix(bmi,".bak")))
                    return std::unexpected("invalid carried compiler baseline");
                source=suffix(bmi,".bak");hash=v[1];
            }
        }
        if(source.empty())return {};
        std::error_code ec;fs::copy_file(source,backup(),fs::copy_options::overwrite_existing,ec);
        if(ec)return std::unexpected("cannot copy the previous BMI");
        const auto time=fs::last_write_time(source,ec);if(ec)return std::unexpected("cannot read the previous BMI time");
        fs::last_write_time(backup(),time,ec);if(ec)return std::unexpected("cannot retain the previous BMI time");
        if(!identity(backup())||mcpp::toolchain::hash_file(backup())!=hash)return std::unexpected("the copied compiler baseline cannot be verified");
        if(!publish(directory/"baseline","mcpp-baseline-v1",{spelling(bmi),hash}))return std::unexpected("cannot certify the previous BMI");
        return {};
    }
    std::expected<bool,std::string> baseline() const {
        auto record=read(directory/"baseline","mcpp-baseline-v1");if(!record)return std::unexpected(record.error());
        if(!*record)return false;
        if((**record).size()!=2||(**record)[0]!=spelling(bmi)||!identity(backup())
            ||(**record)[1]!=mcpp::toolchain::hash_file(backup()))
            return std::unexpected("the certified compiler baseline is incomplete or changed");
        return true;
    }
    std::expected<void,std::string> settle() const {
        if(bmi.empty())return {};
        auto certificate=read(directory/"baseline","mcpp-baseline-v1");
        if(!certificate)return std::unexpected(certificate.error());
        if(!*certificate)return {};
        if(auto certified=baseline();!certified)return std::unexpected(certified.error());
        if(mcpp::build::stage::bmi_equivalent(bmi,backup())&&!copy_atomic(backup(),bmi))
            return std::unexpected("cannot settle an equivalent BMI");
        return {};
    }
    bool publication() const { return publish(directory/"published","mcpp-publication-v1",{}); }
    bool result(int rc) const { return publish(directory/"result","mcpp-result-v1",{std::to_string(rc)}); }
    bool retired(int rc) const {
        auto output=identity(object);if(!output)output=Fields{"missing","missing"};
        return publish(directory/"retired","mcpp-retired-v1",{std::to_string(rc),(*output)[0],(*output)[1]});
    }
    std::expected<void,std::string> register_unit(os::process::owned_unit& unit,const fs::path& actor,const fs::path& reservation) const;
};

std::expected<std::optional<Attempt>,std::string> for_slot(const fs::path& generation,const fs::path& slot) {
    auto pointer=read(suffix(slot,".transaction"),"mcpp-attempt-pointer-v1");
    if(!pointer)return std::unexpected(pointer.error());
    if(!*pointer)return std::optional<Attempt>{};
    if((**pointer).size()!=1)return std::unexpected("invalid compiler transaction pointer");
    const auto directory=named((**pointer)[0]);
    if(directory.parent_path()!=generation/"attempts")return std::unexpected("compiler transaction belongs to another generation");
    auto attempt=Attempt::load(directory);if(!attempt)return std::unexpected(attempt.error());
    return std::optional<Attempt>{std::move(*attempt)};
}
std::expected<void,std::string> validate(const Attempt& attempt) {
    struct Shape {const char* file;const char* tag;std::size_t fields;};
    std::set<std::string_view> present;
    for(const auto& shape:std::array{
        Shape{"baseline","mcpp-baseline-v1",2}, Shape{"unit","mcpp-unit-v1",4},
        Shape{"controller","mcpp-controller-v1",3},
        Shape{"reservation","mcpp-reservation-pointer-v1",1}, Shape{"admitted","mcpp-admitted-v1",0},
        Shape{"published","mcpp-publication-v1",0}, Shape{"retired","mcpp-retired-v1",3},
        Shape{"result","mcpp-result-v1",1}}) {
        auto record=read(attempt.directory/shape.file,shape.tag);
        if(!record)return std::unexpected(record.error());
        if(!*record)continue;
        const auto& fields=**record;
        if(fields.size()!=shape.fields)return std::unexpected("invalid compiler transaction record fields");
        const std::string_view file=shape.file;present.insert(file);
        bool valid=true;
        if(file=="retired")valid=decimal<int>(fields[0])&&output_stamp(fields[1],fields[2]);
        else if(file=="result")valid=decimal<int>(fields[0]);
        else if(file=="controller")valid=decimal<std::uint64_t>(fields[0])&&fields[0]!="0"
            &&decimal<std::uint64_t>(fields[1])&&fields[1]!="0"&&named(fields[2]).is_absolute();
        else if(file=="unit")valid=!fields[0].empty()&&decimal<std::uint64_t>(fields[1])&&fields[1]!="0"
            &&decimal<std::uint64_t>(fields[2])&&fields[2]!="0"&&named(fields[3]).is_absolute();
        else if(file=="baseline")valid=fields[0]==spelling(attempt.bmi)&&digest(fields[1]);
        else if(file=="reservation")valid=named(fields[0]).is_absolute();
        if(!valid)return std::unexpected("invalid compiler transaction record value");
    }
    const auto has=[&](std::string_view file){return present.contains(file);};
    if((has("admitted")&&(!has("unit")||!has("controller")||!has("reservation")))
        ||((has("published")||has("retired"))&&!has("admitted"))
        ||(has("result")&&(!attempt.ordinary||!has("retired"))))
        return std::unexpected("inconsistent compiler transaction stages");
    return {};
}
std::expected<bool,std::string> certified_quiet(const Attempt& attempt) {
    if(auto valid=validate(attempt);!valid)return std::unexpected(valid.error());
    auto retired=read(attempt.directory/"retired","mcpp-retired-v1");
    if(!retired)return std::unexpected(retired.error());
    if(*retired && (**retired).size()!=3)return std::unexpected("invalid compiler retirement certificate");
    if(*retired)return true;
    auto admitted=read(attempt.directory/"admitted","mcpp-admitted-v1");
    if(!admitted)return std::unexpected(admitted.error());
    if(*admitted && !(**admitted).empty())return std::unexpected("invalid compiler admission record");
    return !*admitted; // publication/ACK precede admission; no compiler otherwise
}
std::expected<os::process::unit_state,std::string> observed(const Attempt& attempt,ObserverMap* held=nullptr) {
    const auto quiet=certified_quiet(attempt);if(!quiet)return std::unexpected(quiet.error());
    if(*quiet)return os::process::unit_state::empty;
    auto unit=read(attempt.directory/"unit","mcpp-unit-v1");if(!unit)return std::unexpected(unit.error());
    if(!*unit||(**unit).size()!=4)return std::unexpected("missing admitted compiler unit reference");
    const auto key=spelling(attempt.directory);
    if(held)if(const auto found=held->find(key);found!=held->end()) {
        auto value=found->second.observe();
        return value ? *value : os::process::unit_state::unknown;
    }
    auto observer=os::process::unit_observer::open((**unit)[0]);
    if(!observer)return os::process::unit_state::unknown;
    auto value=observer->observe();return value ? *value : os::process::unit_state::unknown;
}
std::expected<bool,std::string> reservation_available(const fs::path& reservation) {
    auto previous=read(reservation,"mcpp-reservation-v1");if(!previous)return std::unexpected(previous.error());
    if(!*previous)return true;
    if((**previous).size()!=1)return std::unexpected("invalid compiler reservation");
    auto attempt=Attempt::load(named((**previous)[0]));if(!attempt)return std::unexpected(attempt.error());
    const auto state=observed(*attempt);if(!state)return std::unexpected(state.error());
    return *state==os::process::unit_state::empty;
}
bool release_reservation(const fs::path& reservation,const fs::path& attempt) {
    auto current=read(reservation,"mcpp-reservation-v1");
    if(!current)return false;
    if(!*current)return true;
    if((**current).size()!=1)return false;
    if((**current)[0]!=spelling(attempt))return true; // a later, separately locked slot
    std::error_code ec;fs::remove(reservation,ec);return !ec;
}
bool controller_alive(const Attempt& attempt) {
    auto controller=read(attempt.directory/"controller","mcpp-controller-v1");
    if(!controller)return false;
    if(!*controller)return true; // the existing startup bound applies
    if((**controller).size()!=3)return false;
    os::process::program who{};const auto& fields=**controller;
    const auto id=std::from_chars(fields[0].data(),fields[0].data()+fields[0].size(),who.id);
    const auto birth=std::from_chars(fields[1].data(),fields[1].data()+fields[1].size(),who.started_ns);
    return id.ec==std::errc{}&&id.ptr==fields[0].data()+fields[0].size()
        &&birth.ec==std::errc{}&&birth.ptr==fields[1].data()+fields[1].size()
        &&who.id&&who.started_ns&&os::process::alive(who);
}

std::expected<std::string,std::string> receive(os::ipc::connection& connection,std::chrono::milliseconds bound) {
    const auto limit=std::chrono::steady_clock::now()+bound;
    auto exact=[&](std::size_t n)->std::expected<std::string,std::string> {
        std::string bytes;
        while(bytes.size()<n) {
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(limit-std::chrono::steady_clock::now());
            if(left.count()<=0)return std::unexpected("compiler registration acknowledgement timed out");
            auto got=connection.read_for(left,n-bytes.size());
            if(!got||!*got||(**got).empty())return std::unexpected("compiler registration channel closed or timed out");
            bytes+=**got;
        }
        return bytes;
    };
    auto prefix=exact(8);if(!prefix)return std::unexpected(prefix.error());
    std::size_t size=0;const auto parsed=std::from_chars(prefix->data(),prefix->data()+prefix->size(),size,16);
    if(parsed.ec!=std::errc{}||parsed.ptr!=prefix->data()+prefix->size()||size>65536)return std::unexpected("invalid compiler registration frame");
    return exact(size);
}
bool send(os::ipc::connection& connection,std::string_view bytes) {
    return bool(connection.write(std::format("{:08x}",bytes.size())+std::string(bytes)));
}
std::expected<void,std::string> Attempt::register_unit(os::process::owned_unit& unit,
    const fs::path& actor,const fs::path& reservation) const {
    const auto who=os::process::self();if(!who||!who->id||!who->started_ns)return std::unexpected("cannot identify a compiler controller");
    if(!publish(directory/"unit","mcpp-unit-v1",{unit.reference(),std::to_string(who->id),std::to_string(who->started_ns),spelling(actor)}))
        return std::unexpected("cannot record the compiler unit before admission");
    if(!publish(directory/"reservation","mcpp-reservation-pointer-v1",{spelling(reservation)})
        ||!publish(reservation,"mcpp-reservation-v1",{spelling(directory)}))
        return std::unexpected("cannot reserve compiler capacity before admission");
    const auto generation=directory.parent_path().parent_path();
    auto transport=read(generation/"transport","mcpp-unit-transport-v1");
    if(!transport||!*transport||(**transport).size()!=2)return std::unexpected("missing compiler-run registration channel");
    auto connection=os::ipc::connection::connect(named((**transport)[0]),(**transport)[1]);
    if(!connection||!send(*connection,spelling(directory)))return std::unexpected("cannot register the compiler unit");
    auto ack=receive(*connection,std::chrono::seconds(2));
    if(!ack||*ack!="ACK")return std::unexpected(ack ? "compiler-run owner refused unit admission" : ack.error());
    return {};
}

class Witnesses {
public:
    static std::expected<std::unique_ptr<Witnesses>,std::string> start(const fs::path& generation) {
        auto runtime=os::dirs::runtime();if(!runtime)return std::unexpected("no local directory for compiler registration");
        auto result=std::unique_ptr<Witnesses>(new Witnesses);result->generation_=generation;
        std::error_code ec;result->directory_=fs::weakly_canonical(*runtime,ec);
        if(ec)return std::unexpected("cannot identify the compiler registration directory");
        result->name_="mc-"+mcpp::toolchain::hash_string(spelling(generation));
        auto listener=os::ipc::listener::listen(result->directory_,result->name_);
        if(!listener)return std::unexpected("cannot create a compiler registration endpoint: "+listener.error().message());
        result->listener_.emplace(std::move(*listener));
        if(!publish(generation/"transport","mcpp-unit-transport-v1",{spelling(result->directory_),result->name_}))
            return std::unexpected("cannot publish compiler registration transport");
        result->thread_=std::jthread([self=result.get()] {self->serve();});
        return result;
    }
    ~Witnesses() { stop(); }
    void stop() {
        { std::lock_guard guard(admission_);if(stopping_.exchange(true))return; }
        // An ordinary local connection wakes the blocking accept. No periodic
        // filesystem scan, extra process, loopback service or native PID code.
        if(auto wake=os::ipc::connection::connect(directory_,name_))send(*wake,"STOP");
        if(thread_.joinable())thread_.join();
        listener_.reset();
    }
    ObserverMap& observers() {return held_;}
    bool failed() const {return failed_.load();}
private:
    fs::path generation_,directory_;
    std::string name_;
    std::optional<os::ipc::listener> listener_;
    std::jthread thread_;
    std::atomic<bool> stopping_{false},failed_{false};
    std::mutex admission_;
    ObserverMap held_;
    void fail(std::string_view reason) {
        if(!failed_.exchange(true))
            std::println(std::cerr,"mcpp: compiler witness: {}",reason);
    }
    void audit() {
        for(auto it=held_.begin();it!=held_.end();) {
            const auto directory=named(it->first);
            auto retired=read(directory/"retired","mcpp-retired-v1");
            if(!retired) {fail(retired.error());++it;continue;}
            if(*retired&&(**retired).size()!=3) {fail("invalid retirement certificate");++it;continue;}
            if(*retired) {it=held_.erase(it);continue;}
            auto attempt=Attempt::load(directory);
            if(!attempt) {fail(attempt.error());++it;continue;}
            if(!controller_alive(*attempt)) {
                // Retirement can be published between the first read and the
                // controller's exit. Observe it again before reporting loss.
                retired=read(directory/"retired","mcpp-retired-v1");
                if(!retired)fail(retired.error());
                else if(*retired&&(**retired).size()!=3)fail("invalid retirement certificate");
                else if(*retired) {it=held_.erase(it);continue;}
                else fail("controller ended without retirement certificate");
            }
            ++it;
        }
    }
    void serve() {
        auto next_audit=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        while(!stopping_.load()) {
            if(std::chrono::steady_clock::now()>=next_audit) {
                audit();next_audit=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            }
            auto accepted=listener_->accept_for(std::chrono::milliseconds(500));
            if(!accepted||!*accepted)continue;
            auto connection=std::move(**accepted);
            auto request=receive(connection,std::chrono::milliseconds(500));
            if(!request||stopping_.load())continue;
            const auto attempt=named(*request);
            if(attempt.parent_path()!=generation_/"attempts") {send(connection,"REFUSED");continue;}
            auto prepared=Attempt::load(attempt);
            auto unit=read(attempt/"unit","mcpp-unit-v1");
            if(!prepared||!unit||!*unit||(**unit).size()!=4) {send(connection,"REFUSED");continue;}
            const auto actor=named((**unit)[3]);
            if(actor.parent_path()!=generation_/"actors") {send(connection,"REFUSED");continue;}
            auto lease=os::fs::lock::acquire(actor,os::fs::lock::kind::exclusive,false);
            if(lease||lease.error().code!=os::errc::again||!controller_alive(*prepared)) {send(connection,"REFUSED");continue;}
            auto retained=os::process::unit_observer::open((**unit)[0]);
            if(!retained) {send(connection,"REFUSED");continue;}
            std::lock_guard guard(admission_);
            if(stopping_.load()||failed_.load()) {send(connection,"REFUSED");continue;}
            held_.insert_or_assign(*request,std::move(*retained));
            send(connection,"ACK");
        }
    }
};

std::expected<bool,std::string> recover(const Attempt& attempt,ObserverMap* held=nullptr) {
    const auto state=observed(attempt,held);if(!state)return std::unexpected(state.error());
    if(*state!=os::process::unit_state::empty)return false;
    auto retired=read(attempt.directory/"retired","mcpp-retired-v1");
    auto result=attempt.ordinary ? read(attempt.directory/"result","mcpp-result-v1") : Record{std::optional<Fields>{}};
    auto published=read(attempt.directory/"published","mcpp-publication-v1");
    auto admitted=read(attempt.directory/"admitted","mcpp-admitted-v1");
    if(!retired||!result||!published||!admitted)return std::unexpected("cannot validate compiler transaction completion");
    if(*published&&!(**published).empty())return std::unexpected("invalid BMI publication certificate");
    if(*published&&!*admitted)return std::unexpected("BMI publication without registered compiler admission");
    bool committed=false;
    if(*retired) {
        if((**retired).size()!=3)return std::unexpected("invalid compiler retirement certificate");
        const auto& status=(**retired)[0];int rc=0;
        const auto parsed=std::from_chars(status.data(),status.data()+status.size(),rc);
        if(parsed.ec!=std::errc{}||parsed.ptr!=status.data()+status.size())return std::unexpected("invalid retired compiler status");
        if(attempt.ordinary) {
            if(*result&&((**result).size()!=1||(**result)[0]!=status))return std::unexpected("compiler result does not match retirement");
            committed=rc==0&&result->has_value();
        } else {
            std::ifstream completed(suffix(attempt.slot,".rc"));int value=0;std::string extra;
            committed=rc==0&&bool(completed>>value)&&!(completed>>extra)&&value==rc;
        }
        const auto output=identity(attempt.object);
        committed=committed&&output&&(*output)[0]==(**retired)[1]&&(*output)[1]==(**retired)[2];
        if(!attempt.bmi.empty()) {
            const auto bmi=identity(attempt.bmi);
            const bool changed=bmi&&(!attempt.before_present||(*bmi)[0]!=attempt.before_size||(*bmi)[1]!=attempt.before_time);
            committed=committed&&bmi&&(published->has_value()||changed);
        }
    }
    std::error_code ec;
    if(!committed) {
        const auto baseline=attempt.baseline();if(!baseline)return std::unexpected(baseline.error());
        if(*baseline) {
            if(*published) {
                // An early BMI edge may already be in Ninja's log. Make it
                // absent to force retry, while retaining a certified baseline
                // for that retry's own failure/semantic-equivalence handling.
                auto certificate=read(attempt.directory/"baseline","mcpp-baseline-v1");
                if(!certificate||!*certificate)return std::unexpected("lost compiler baseline certificate");
                if(!copy_atomic(attempt.backup(),suffix(attempt.bmi,".bak"))
                    ||!publish(suffix(attempt.bmi,".bak.cert"),"mcpp-carried-baseline-v1",{spelling(attempt.bmi),(**certificate)[1]}))
                    return std::unexpected("cannot retain the compiler baseline before invalidation");
                fs::remove(attempt.bmi,ec);if(ec)return std::unexpected("cannot invalidate an abandoned BMI");
            } else if(!copy_atomic(attempt.backup(),attempt.bmi))return std::unexpected("cannot restore the previous verified BMI");
        } else if(attempt.before_present) {
            // No certified copy can precede admission when a previous BMI
            // existed. Before admission, the original is still the baseline.
            if(*admitted)return std::unexpected("an admitted compiler has no certified previous BMI");
            const auto old=identity(attempt.bmi);
            if(!old||(*old)[0]!=attempt.before_size||(*old)[1]!=attempt.before_time
                ||mcpp::toolchain::hash_file(attempt.bmi)!=attempt.before_hash)
                return std::unexpected("the unmodified compiler baseline cannot be verified");
        } else if(!attempt.bmi.empty()) {fs::remove(attempt.bmi,ec);if(ec)return std::unexpected("cannot invalidate an abandoned BMI");}
        if(*admitted)for(const auto& path:{attempt.object,attempt.dep})if(!path.empty()) {
            fs::remove(path,ec);if(ec)return std::unexpected("cannot invalidate an abandoned compiler output");
        }
        if(!attempt.slot.empty())for(std::string_view tail:{".rc",".rc.tmp"}) {
            fs::remove(suffix(attempt.slot,tail),ec);if(ec)return std::unexpected("cannot invalidate an abandoned compiler status");
        }
    } else if(!attempt.bmi.empty()) {
        const auto object=identity(attempt.object),bmi=identity(attempt.bmi);
        if(!object||!bmi||!publish(suffix(attempt.slot,".completed"),"mcpp-completed-unit-v1",{
            spelling(attempt.bmi),spelling(attempt.object),(*object)[0],(*object)[1],(*bmi)[0],(*bmi)[1]}))
            return std::unexpected("cannot retain the verified compiler cache result");
        // Carried baselines are obsolete only after a verified successful
        // result. Normal completion keeps every actual output untouched.
        fs::remove(suffix(attempt.bmi,".bak.cert"),ec);if(ec)return std::unexpected("cannot retire the carried baseline certificate");
        fs::remove(suffix(attempt.bmi,".bak"),ec);if(ec)return std::unexpected("cannot retire the carried baseline");
    }
    auto reservation=read(attempt.directory/"reservation","mcpp-reservation-pointer-v1");
    if(!reservation)return std::unexpected(reservation.error());
    if(*reservation) {
        if((**reservation).size()!=1||!release_reservation(named((**reservation)[0]),attempt.directory))
            return std::unexpected("cannot retire the compiler reservation");
    }
    return true;
}
// A completed generation leaves this quiet-unit proof beside the cached
// status. A later missing object can invalidate its BMI without guessing that
// an old controller's absence means no writer, even after journals are retired.
std::expected<std::optional<int>,std::string> cached_result(const fs::path& slot,const fs::path& requested_object) {
    auto completed=read(suffix(slot,".completed"),"mcpp-completed-unit-v1");
    if(!completed)return std::unexpected(completed.error());
    if(!*completed)return std::optional<int>{};
    const auto& v=**completed;
    auto object=output_identity(requested_object);
    if(v.size()!=6||!object||v[1]!=spelling(*object)||!named(v[0]).is_absolute())
        return std::unexpected("invalid completed compiler output identity");
    const auto current=identity(*object);
    if(current&&(*current)[0]==v[2]&&(*current)[1]==v[3])return std::optional<int>{0};
    const auto bmi=named(v[0]);const auto current_bmi=identity(bmi);
    if(current_bmi) {
        if((*current_bmi)[0]!=v[4]||(*current_bmi)[1]!=v[5])return std::unexpected("cached BMI identity changed outside its completed attempt");
        if(!copy_atomic(bmi,suffix(bmi,".bak"))
            ||!publish(suffix(bmi,".bak.cert"),"mcpp-carried-baseline-v1",{spelling(bmi),mcpp::toolchain::hash_file(bmi)}))
            return std::unexpected("cannot retain the cached BMI before retry");
    }
    std::error_code ec;
    for(const auto& path:{bmi,suffix(slot,".rc"),suffix(slot,".completed")}) {
        fs::remove(path,ec);if(ec)return std::unexpected("cannot invalidate a missing cached compiler output");
    }
    return std::optional<int>{1};
}

std::expected<void,std::string> recover_generation(const fs::path& generation,ObserverMap* held=nullptr) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        bool pending=false;std::error_code ec;
        const auto directory=generation/"attempts";
        if(!fs::exists(directory,ec))return ec ? std::unexpected("cannot inspect compiler transactions") : std::expected<void,std::string>{};
        fs::directory_iterator entries(directory,ec);if(ec)return std::unexpected("cannot enumerate compiler transactions");
        for(const auto& entry:entries) {
            auto prepared=read(entry.path()/"prepared","mcpp-attempt-v1");
            if(!prepared)return std::unexpected(prepared.error());
            if(!*prepared) {
                // Preparation's atomic record precedes every modification.
                // An empty, unreferenced staging directory has no outputs.
                if(fs::exists(entry.path()/"unit",ec)||fs::exists(entry.path()/"admitted",ec))return std::unexpected("compiler unit without prepared transaction");
                continue;
            }
            auto attempt=Attempt::load(entry.path());if(!attempt)return std::unexpected(attempt.error());
            const auto done=recover(*attempt,held);if(!done)return std::unexpected(done.error());
            if(!*done)pending=true;
        }
        if(!pending)return {};
        if(std::chrono::steady_clock::now()>=limit)return std::unexpected("previous compiler unit is occupied or unknown; reservation and outputs were retained");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
// Admit Ninja only after certified cached outputs have been checked.
// This reads existing completion certificates; it creates no source-dirty index.
std::expected<void,std::string> admit_cached_outputs(const fs::path& output_directory);
} // namespace mcpp::build::schedule::transaction
