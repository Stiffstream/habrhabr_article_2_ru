#include <common/stuff.hpp>
#include <common/io_agent.hpp>

#include <list>

// Имитация агентов-checker-ов конкретных частей сообщения.
// Поскольку все имитаторы будут одинаковыми, используем шаблон,
// который будет параметризоваться типами-тегами.
struct headers_checker_tag {
  using data_type = vector< string >;
};
struct body_checker_tag {
  using data_type = string;
};
struct attach_checker_tag {
  using data_type = vector< string >;
};

unsigned int checker_imit_counter() {
  static atomic< unsigned int > counter{};
  return ++counter;
}

template< typename TAG >
class checker_template : public agent_t {
public :
  struct result { check_status status_; };

  checker_template( context_t ctx, mbox_t reply_to, typename TAG::data_type )
    : agent_t(ctx), reply_to_(move(reply_to) )
  {}

  virtual void so_evt_start() override {
    auto i = checker_imit_counter();
    if( !(i % 17) )
      // На каждый 17-й вызов вообще ничего не возвращаем.
      // У email_analyzer-а должен сработать тайм-аут.
      return;

    // Определяем, какой ответ следует вернуть.
    check_status status{ check_status::safe };
    if( !(i % 11) )
      status = check_status::suspicious;
    else if( !(i % 19) )
      status = check_status::dangerous;

    send_delayed< result >(
        this->so_environment(), reply_to_,
        chrono::milliseconds( 50 + (i % 7) * 110 ),
        status );
  }

private :
  mbox_t reply_to_;
};

using email_headers_checker = checker_template< headers_checker_tag >;
using email_body_checker = checker_template< body_checker_tag >;
using email_attach_checker = checker_template< attach_checker_tag >;

class email_analyzer : public agent_t {
  state_t st_wait_io{ this };
  state_t st_wait_checkers{ this };

  state_t st_finishing{ this };
  state_t st_failure{  initial_substate_of{ st_finishing } };
  state_t st_success{ substate_of{ st_finishing } };

public :
  email_analyzer( context_t ctx,
    string email_file,
    mbox_t reply_to )
    : agent_t(ctx), email_file_(move(email_file)), reply_to_(move(reply_to))
  {}

  virtual void so_define_agent() override {
    st_wait_io
      .event( &email_analyzer::on_load_succeed )
      .event( &email_analyzer::on_load_failed )
      // Назначаем тайм-аут для ожидания ответа.
      .time_limit( 1500ms, st_failure );

    st_wait_checkers
      .event( [this]( const email_headers_checker::result & msg ) {
          on_checker_result( msg.status_ );
        } )
      .event( [this]( const email_body_checker::result & msg ) {
          on_checker_result( msg.status_ );
        } )
      .event( [this]( const email_attach_checker::result & msg ) {
          on_checker_result( msg.status_ );
        } )
      // Еще один тайм-аут для ответов.
      .time_limit( 750ms, st_failure );

    // Для состояний, которые отвечают за завершение работы,
    // нужно определить только обработчики входа.
    st_finishing.on_enter( [this]{ so_deregister_agent_coop_normally(); } );
    st_failure.on_enter( [this]{
        send< check_result >( reply_to_, email_file_, status_ );
      } );
    st_success.on_enter( [this]{
        send< check_result >( reply_to_, email_file_, check_status::safe );
      } );
  }

  virtual void so_evt_start() override {
    // Начинаем работать в состоянии по умолчанию, поэтому
    // нужно принудительно перейти в нужное состояние.
    st_wait_io.activate();

    // При старте сразу же отправляем запрос IO-агенту для загрузки
    // содержимого email файла.
    send< load_email_request >(
        so_environment().create_mbox( "io_agent" ),
        email_file_,
        so_direct_mbox() );
  }

private :
  const string email_file_;
  const mbox_t reply_to_;

  // Храним последний отрицательный результат для того, чтобы отослать
  // его при входе в состояние st_failure.
  check_status status_{ check_status::check_failure };

  int checks_passed_{};

  void on_load_succeed( const load_email_succeed & msg ) {
    // Меняем состояние т.к. переходим к следующей операции.
    st_wait_checkers.activate();

    try {
      auto parsed_data = parse_email( msg.content_ );
      introduce_child_coop( *this,
        // Агенты-checker-ы будут работать на своем собственном
        // thread-pool-диспетчере, который был создан заранее
        // под специальным именем.
        disp::thread_pool::create_disp_binder(
            "checkers", disp::thread_pool::bind_params_t{} ),
        [&]( coop_t & coop ) {
          coop.make_agent< email_headers_checker >(
              so_direct_mbox(), parsed_data->headers() );
          coop.make_agent< email_body_checker >(
              so_direct_mbox(), parsed_data->body() );
          coop.make_agent< email_attach_checker >(
              so_direct_mbox(), parsed_data->attachments() );
        } );
    }
    catch( const exception & ) {
      st_failure.activate();
    }
  }

  void on_load_failed( const load_email_failed & ) {
    st_failure.activate();
  }

  void on_checker_result( check_status status ) {
    // На первом же неудачном результате прерываем свою работу.
    if( check_status::safe != status ) {
      status_ = status;
      st_failure.activate();
    }
    else {
      ++checks_passed_;
      if( 3 == checks_passed_ )
        // Все результаты получены. Можно завершать проверку с
        // положительным результатом.
        st_success.activate();
    }
  }
};

class analyzer_manager final : public agent_t {
  struct try_create_next_analyzer : public signal_t {};
  struct analyzer_finished : public signal_t {};

  // Потребуется еще один сигнал для таймера проверки времени жизни
  // заявки в списке ожидания.
  struct check_lifetime : public signal_t {};

  // Кроме того, нам потребуется другая структура для хранения заявки
  // в списке ожидания. Кроме самой заявки нужно будет хранить еще
  // и время поступления в список ожидания.
  using clock = chrono::steady_clock;
  struct pending_request {
    clock::time_point stored_at_;
    check_request request_;
  };

public :
  analyzer_manager( context_t ctx )
    : agent_t( ctx )
    , analyzers_disp_(
        disp::thread_pool::create_private_disp(
            so_environment(),
            thread::hardware_concurrency() ) )
  {
    so_subscribe_self()
      .event( &analyzer_manager::on_new_check_request )
      .event< try_create_next_analyzer >( &analyzer_manager::on_create_new_analyzer )
      .event< analyzer_finished >( &analyzer_manager::on_analyzer_finished )
      // Для обработки таймера нам нужен еще одно событие-обработчик.
      .event< check_lifetime >( &analyzer_manager::on_check_lifetime );
  }

  // Используем стартовый метод для того, чтобы запустить периодический таймер.
  virtual void so_evt_start() override {
    // Для периодических таймеров нужно сохранять возвращаемый timer_id,
    // иначе таймер будет автоматически отменен.
    check_lifetime_timer_ = send_periodic< check_lifetime >( *this, 500ms, 500ms );
  }

private :
  const size_t max_parallel_analyzers_{ 16 };
  size_t active_analyzers_{ 0 };

  disp::thread_pool::private_dispatcher_handle_t analyzers_disp_;

  // Ограничение на время пребывания заявки в списке ожидания.
  const chrono::seconds max_lifetime_{ 10 };
  // Идентификатор таймера для периодического сигнала check_lifetime.
  timer_id_t check_lifetime_timer_;

  list< pending_request > pending_requests_;

  void on_new_check_request( const check_request & msg ) {
    // Теперь при сохранении фиксируем время.
    pending_requests_.push_back( pending_request{ clock::now(), msg } );
    send< try_create_next_analyzer >( *this );
  }

  void on_create_new_analyzer() {
    if( active_analyzers_ >= max_parallel_analyzers_ )
      return;

    lauch_new_analyzer();

    if( !pending_requests_.empty()
        && active_analyzers_ < max_parallel_analyzers_ )
      send< try_create_next_analyzer >( *this );
  }

  void on_analyzer_finished() {
    --active_analyzers_;

    if( !pending_requests_.empty() )
      lauch_new_analyzer();
  }

  void on_check_lifetime() {
    // Продолжать просмотр списка можно пока в нем есть элементы, которые
    // подлежат изъятию.
    while( !pending_requests_.empty() &&
      pending_requests_.front().stored_at_ + max_lifetime_ < clock::now() )
    {
      // Отсылаем неудачный результат проверки email-а самостоятельно.
      send< check_result >(
        pending_requests_.front().request_.reply_to_,
        pending_requests_.front().request_.email_file_,
        check_status::check_timedout );
      pending_requests_.pop_front();
    } 
  }
  
  void lauch_new_analyzer() {
    introduce_child_coop( *this,
      analyzers_disp_->binder( disp::thread_pool::bind_params_t() ),
      [this]( coop_t & coop ) {
        coop.make_agent< email_analyzer >(
          pending_requests_.front().request_.email_file_,
          pending_requests_.front().request_.reply_to_ );

        coop.add_dereg_notificator(
          [this]( environment_t &, const string &, const coop_dereg_reason_t & ) {
            send< analyzer_finished >( *this );
          } );
      } );

    ++active_analyzers_;

    pending_requests_.pop_front();
  }
};

void do_imitation() {
  so_5::launch( []( environment_t & env ) {
    // Запускаем IO-агента, который уже должен работать к моменту,
    // когда появятся первые агенты email_analyzer.
    make_io_agent( env );

    // Теперь можно запускать агента-менеджера.
    mbox_t checker_mbox;
    env.introduce_coop( [&]( coop_t & coop ) {
      auto manager = coop.make_agent< analyzer_manager >();
      // mbox агента-менеджера потребуется для формирования потока запросов.
      checker_mbox = manager->so_direct_mbox();
    } );

    // Следующей кооперацией будет кооперация с агентом-имитатором запросов.
    // Запускаем имитатор запроса на собственной рабочей нити, дабы обработка
    // его сообщений выполнялась независимо от обработки сообщений
    // агента-менеджера.
    env.introduce_coop(
      disp::one_thread::create_private_disp( env )->binder(),
      [checker_mbox]( coop_t & coop ) {
        coop.make_agent< requests_initiator >( checker_mbox, 5000u );
      } );
  },
  // Нужно создать диспетчера, на котором будут работать агенты-checker-ы.
  []( environment_params_t & params ) {
    params.add_named_dispatcher(
        // По этому имени затем агенты-checker-ы будут привязываться
        // к данному диспетчеру.
        "checkers",
        // Для демонстрации отводим агентам-checker-ам всего
        // две рабочие нити.
        disp::thread_pool::create_disp( 2 ) );
  } );
}

int main() {
  try {
    do_imitation();
    return 0;
  }
  catch( const exception & x ) {
    cerr << "Oops! " << x.what() << endl;
  }

  return 2;
}

