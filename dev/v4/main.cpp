#include <common/stuff.hpp>

#include <list>

// Агент для анализа содержимого одного email-а.
// Получает все нужные ему параметры в конструкторе,
// выполняет все свои действия в единственном методе so_evt_start.
class email_analyzer : public agent_t {
public :
  email_analyzer( context_t ctx,
    // Имя файла с email для анализа.
    string email_file,
    // Куда нужно отослать результат анализа.
    mbox_t reply_to )
    : agent_t(ctx), email_file_(move(email_file)), reply_to_(move(reply_to))
  {}

  virtual void so_evt_start() override {
    try {
      // Стадии обработки обозначаем лишь схематично.
      auto raw_data = load_email_from_file( email_file_ );
      auto parsed_data = parse_email( raw_data );
      auto status = check_headers( parsed_data->headers() );
      if( check_status::safe == status )
        status = check_body( parsed_data->body() );
      if( check_status::safe == status )
        status = check_attachments( parsed_data->attachments() );
      send< check_result >( reply_to_, email_file_, status );
    }
    catch( const exception & ) {
      // В случае какой-либо ошибки отсылаем статус о невозможности
      // проверки файла с email-ом по техническим причинам.
      send< check_result >(
          reply_to_, email_file_, check_status::check_failure );
    }
    // Больше мы не нужны, поэтому дерегистрируем кооперацию,
    // в которой находимся.
    so_deregister_agent_coop_normally();
  }

private :
  const string email_file_;
  const mbox_t reply_to_;
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
        disp::thread_pool::create_private_disp( so_environment(), 16 ) )
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

int main() {
  try {
    do_imitation< analyzer_manager >( 5000 );
    return 0;
  }
  catch( const exception & x ) {
    cerr << "Oops! " << x.what() << endl;
  }

  return 2;
}

