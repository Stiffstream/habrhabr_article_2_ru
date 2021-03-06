#include <common/stuff.hpp>
#include <common/io_agent.hpp>

#include <list>

class email_analyzer : public agent_t {
  // Этот сигнал потребуется для того, чтобы отслеживать отсутствие
  // ответа от IO-агента в течении разумного времени.
  struct io_agent_response_timeout : public signal_t {};

public :
  email_analyzer( context_t ctx,
    string email_file,
    mbox_t reply_to )
    : agent_t(ctx), email_file_(move(email_file)), reply_to_(move(reply_to))
  {}

  virtual void so_define_agent() override {
    so_subscribe_self()
      .event( &email_analyzer::on_load_succeed )
      .event( &email_analyzer::on_load_failed )
      // Добавляем еще обработку тайм-аута на ответ IO-агента.
      .event< io_agent_response_timeout >( &email_analyzer::on_io_timeout );
  }

  virtual void so_evt_start() override {
    // При старте сразу же отправляем запрос IO-агенту для загрузки
    // содержимого email файла.
    send< load_email_request >(
        so_environment().create_mbox( "io_agent" ),
        email_file_,
        so_direct_mbox() );

    // И сразу же начинам отсчет тайм-аута для ответа от IO-агента.
    send_delayed< io_agent_response_timeout >( *this, 1500ms );
  }

private :
  const string email_file_;
  const mbox_t reply_to_;

  void on_load_succeed( const load_email_succeed & msg ) {
    try {
      auto parsed_data = parse_email( msg.content_ );
      auto status = check_headers( parsed_data->headers() );
      if( check_status::safe == status )
        status = check_body( parsed_data->body() );
      if( check_status::safe == status )
        status = check_attachments( parsed_data->attachments() );
      send< check_result >( reply_to_, email_file_, status );
    }
    catch( const exception & ) {
      send< check_result >(
          reply_to_, email_file_, check_status::check_failure );
    }
    so_deregister_agent_coop_normally();
  }

  void on_load_failed( const load_email_failed & ) {
    send< check_result >(
        reply_to_, email_file_, check_status::check_failure );
    so_deregister_agent_coop_normally();
  }

  void on_io_timeout() {
    // Ведем себя точно так же, как и при ошибке ввода-вывода.
    send< check_result >(
        reply_to_, email_file_, check_status::check_failure );
    so_deregister_agent_coop_normally();
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
  so_5::launch( [=]( environment_t & env ) {
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

